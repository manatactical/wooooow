#include "Engine/Source/ModernAssets/Db2/Db2Table.h"

#include "Engine/Source/ModernAssets/ModernAssetsFile.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"

#include <algorithm>
#include <unordered_set>

// Ported from WarcraftXL wxl-db2 api/Db2.cpp (GPL-3.0-or-later, Copyright (C) 2026 WarcraftXL).

namespace modern::db2
{
    namespace
    {
        bool Fail(std::string* error, std::string message)
        {
            if (error) *error = std::move(message);
            return false;
        }

        /// The archive path a definition names: a bare filename lives under DBFilesClient.
        std::string ArchivePath(std::string_view filename)
        {
            std::string path(filename);
            std::replace(path.begin(), path.end(), '/', '\\');
            if (path.find('\\') == std::string::npos) path.insert(0, "DBFilesClient\\");
            return path;
        }

        bool HasField(const Definition& definition, std::string_view name)
        {
            if (name == "@id" || name == "@parent") return true;
            return std::any_of(definition.fields.begin(), definition.fields.end(),
                               [name](const Field& field) { return field.name == name; });
        }
    }

    bool Table::Load(const Definition& definition, std::string* error)
    {
        std::vector<uint8_t> bytes;
        if (!modern::assets::ReadWholeFile(ArchivePath(definition.filename).c_str(), bytes))
        {
            if (error) *error = "DB2: unable to read " + ArchivePath(definition.filename);
            return false;
        }
        return LoadBytes(definition, bytes.data(), bytes.size(), error);
    }

    bool Table::LoadBytes(const Definition& definition, const void* bytes, size_t size,
                          std::string* error)
    {
        name_.clear();
        filename_.clear();
        fields_.clear();
        relations_.clear();
        table_ = {};
        loaded_ = false;
        if (error) error->clear();
        if (definition.name.empty() || definition.filename.empty())
            return Fail(error, "DB2: table name and filename are required");
        if (definition.fields.empty())
            return Fail(error, "DB2: " + std::string(definition.name) + " has no physical fields");

        std::vector<wdc5::FieldShape> shapes;
        shapes.reserve(definition.fields.size());
        fields_.reserve(definition.fields.size());
        for (size_t i = 0; i < definition.fields.size(); ++i)
        {
            const Field& field = definition.fields[i];
            if (field.name.empty() || !fields_.emplace(std::string(field.name), i).second)
                return Fail(error, "DB2: " + std::string(definition.name) +
                                   " has an empty or duplicate field name");
            shapes.push_back(wdc5::FieldShape{ std::max<uint16_t>(1, field.elements) });
        }

        for (const Relation& relation : definition.relations)
        {
            RelationBinding binding{ relation.source, kNoField, relation.sourceElement };
            if (relation.source == RelationSource::Field)
            {
                const auto field = fields_.find(std::string(relation.sourceField));
                if (field == fields_.end())
                    return Fail(error, "DB2: relation " + std::string(relation.name) +
                                       " names an unknown source field");
                binding.field = field->second;
                if (binding.element >= std::max<uint16_t>(1, definition.fields[binding.field].elements))
                    return Fail(error, "DB2: relation " + std::string(relation.name) +
                                       " has an invalid array element");
            }
            if (relation.name.empty() || !relations_.emplace(std::string(relation.name), binding).second)
                return Fail(error, "DB2: " + std::string(definition.name) +
                                   " has an empty or duplicate relation name");
        }

        if (!table_.Load(bytes, size, shapes, definition.layoutHash, error)) return false;

        name_.assign(definition.name);
        filename_.assign(definition.filename);
        loaded_ = true;
        return true;
    }

    uint32_t Table::Value(const wdc5::Row& row, size_t field, size_t element) const noexcept
    {
        return table_.Value(row, field, element);
    }

    uint32_t Table::Value(const wdc5::Row& row, std::string_view field, size_t element) const noexcept
    {
        const size_t index = FieldIndex(field);
        return index == kNoField ? 0 : table_.Value(row, index, element);
    }

    size_t Table::FieldIndex(std::string_view field) const noexcept
    {
        const auto it = fields_.find(std::string(field));
        return it == fields_.end() ? kNoField : it->second;
    }

    size_t Table::ElementCount(std::string_view field) const noexcept
    {
        const size_t index = FieldIndex(field);
        return index == kNoField ? 0 : table_.ElementCount(index);
    }

    uint32_t Table::RelationKey(const wdc5::Row& row, std::string_view relation) const noexcept
    {
        const auto it = relations_.find(std::string(relation));
        if (it == relations_.end()) return 0;
        switch (it->second.source)
        {
            case RelationSource::RowId: return row.id;
            case RelationSource::ParentId: return row.parentId;
            case RelationSource::Field: return table_.Value(row, it->second.field, it->second.element);
        }
        return 0;
    }

    bool ValidateDefinitions(const std::vector<Definition>& definitions, std::string* error)
    {
        if (error) error->clear();

        std::unordered_set<std::string> names;
        for (const Definition& definition : definitions)
        {
            if (definition.name.empty() || definition.filename.empty() || definition.fields.empty())
                return Fail(error, "DB2: every definition needs a name, filename, and physical fields");
            if (!names.emplace(definition.name).second)
                return Fail(error, "DB2: duplicate table definition " + std::string(definition.name));

            std::unordered_set<std::string> fields;
            for (const Field& field : definition.fields)
                if (field.name.empty() || !fields.emplace(field.name).second)
                    return Fail(error, "DB2: duplicate or empty field in " + std::string(definition.name));
        }

        for (const Definition& definition : definitions)
        {
            for (const Relation& relation : definition.relations)
            {
                if (relation.name.empty() || relation.targetTable.empty())
                    return Fail(error, "DB2: incomplete relation in " + std::string(definition.name));
                if (relation.source == RelationSource::Field &&
                    !HasField(definition, relation.sourceField))
                    return Fail(error, "DB2: relation source field is absent from " +
                                       std::string(definition.name));
                const auto target = std::find_if(definitions.begin(), definitions.end(),
                    [&relation](const Definition& d) { return d.name == relation.targetTable; });
                if (target == definitions.end())
                    return Fail(error, "DB2: relation target table " +
                                       std::string(relation.targetTable) + " is not declared");
                if (!HasField(*target, relation.targetField))
                    return Fail(error, "DB2: relation target field is absent from " +
                                       std::string(target->name));
            }
        }
        return true;
    }
}
