#include "Liquid.h"
#include <algorithm>
#include <cstdlib>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxLight.h"
#include "Engine/Source/ModernWater/ModernWater.h"
#include "Engine/Source/ModernWater/water_inputs.h"
#include "Engine/Source/ModernWater/water_renderer.h"
#include "Engine/Source/Storm/H/storm.h"
#include <d3d9.h>

// STATIC: 0x00B23F6C
Liquid::CRenderer::DrawListSorter Liquid::CRenderer::s_drawListSorter[2] = {
    Liquid::CRenderer::CompareOpaque,
    Liquid::CRenderer::CompareTransparent
};

// VERIFY: 3d910c8b manual
// OFFSET: 0x00791910
Liquid::CRenderer::~CRenderer() {
}

// VERIFY: c8176254 manual
// OFFSET: 0x008A1980
int32_t Liquid::CRenderer::CompareOpaque(const void* a, const void* b) {
    const Liquid::CInstance* l = static_cast<const Liquid::CInstance*>(a);
    const Liquid::CInstance* r = static_cast<const Liquid::CInstance*>(b);

    if (l->m_material < r->m_material) {
        return -1;
    }

    if (l->m_material > r->m_material) {
        return 1;
    }

    if (l->m_settings < r->m_settings) {
        return -1;
    }

    if (l->m_settings > r->m_settings) {
        return 1;
    }

    if (l->m_geomFactory < r->m_geomFactory) {
        return -1;
    }

    if (l->m_geomFactory > r->m_geomFactory) {
        return 1;
    }

    if (l->m_transform.d3 > r->m_transform.d3) {
        return 1;
    }

    if (l->m_transform.d3 < r->m_transform.d3) {
        return -1;
    }

    return 0;
}

// VERIFY: abfe3e46 manual
// OFFSET: 0x008A19E0
int32_t Liquid::CRenderer::CompareTransparent(const void* a, const void* b) {
    const Liquid::CInstance* l = static_cast<const Liquid::CInstance*>(a);
    const Liquid::CInstance* r = static_cast<const Liquid::CInstance*>(b);

    if (l->m_material < r->m_material) {
        return -1;
    }

    if (l->m_material > r->m_material) {
        return 1;
    }

    if (l->m_settings < r->m_settings) {
        return -1;
    }

    if (l->m_settings > r->m_settings) {
        return 1;
    }

    if (l->m_geomFactory < r->m_geomFactory) {
        return -1;
    }

    if (l->m_geomFactory > r->m_geomFactory) {
        return 1;
    }

    if (l->m_transform.d3 > r->m_transform.d3) {
        return -1;
    }

    if (l->m_transform.d3 < r->m_transform.d3) {
        return 1;
    }

    return 0;
}

// VERIFY: f6fe74d2 manual
// OFFSET: 0x008A20C0
void Liquid::CRenderer::Draw(Liquid::CInstance* instance) {
    if (!instance->m_queued) {
        // Not in the original binary. The modern water pass runs only in the transparent flush, so water whose material
        // is opaque would be drawn by the client's own shader in the opaque flush and never shaded. Shaded water is
        // always queued with the transparent draws.
        const bool shaded = GlobalWaterRenderer().Active() && ModernWater::TagLiquidInstance(instance);
        this->m_instances[shaded || instance->m_settings->m_transparent != 0].New(instance);
        instance->m_queued = 1;
    }
}

// VERIFY: d17a0633 manual
// OFFSET: 0x008A2240
void Liquid::CRenderer::Flush(const C3Vector& cameraPos, uint32_t pass) {
    const uint32_t limit = GxGetMaxLights();
    CGxLight lights[Gx_MAX_LIGHTS];

    for (uint32_t i = 0; i < limit; i++) {
        g_theGxDevicePtr->Light(i, lights[i]);
    }

    TSGrowableArray<Liquid::CInstance*>& instances = this->m_instances[pass];
    qsort(instances.m_data, instances.Count(), sizeof(Liquid::CInstance*), Liquid::CRenderer::s_drawListSorter[pass]);

    uint32_t count = instances.Count();

    // Not in the original binary. The transparent pass is where the water surfaces are, and it is the moment the modern
    // water pass needs: everything opaque and the sky are already in the frame, and the water itself is not yet.
    //
    // The instances are already sorted by material and then by settings, so the draws of one water class are
    // contiguous. That is what lets each class be shaded as soon as its own run of draws is down: with only that
    // class in the frame since the last shade, the depth difference is exactly that class's pixels, so no per-class
    // mask (and no stencil) is needed to keep two classes from painting over each other.
    WaterRenderer& water = GlobalWaterRenderer();
    IDirect3DDevice9* device = static_cast<IDirect3DDevice9*>(g_theGxDevicePtr->DeviceNativeD3d9Device());
    bool armed = false;

    if (pass == 1 && water.Active()) {
        WaterFrameInputs frameInputs;

        if (ModernWater::BuildFrameInputs(frameInputs)) {
            WaterContactFrame contacts;
            ModernWater::ReadContacts(frameInputs, contacts);
            water.SetContacts(contacts);
            armed = water.Begin(device, this, frameInputs, ModernWater::WaterSettings());
        }
    }

    // The list is sorted by material first, so one water class can come in several runs with another class between
    // them (river, ocean, river). Each run is shaded against the depth the earlier runs left, and a plane that overlaps
    // an earlier run at the same depth wins no pixels there, so the later run of that class lost the overlap and it
    // flickered away with the camera angle. Grouping the draws by class makes every class one run; the sort is stable,
    // so the order inside a class is the client's own.
    if (armed) {
        std::stable_sort(instances.m_data, instances.m_data + count,
                         [](const Liquid::CInstance* l, const Liquid::CInstance* r) {
                             return ModernWater::ClassifyLiquidInstance(l) < ModernWater::ClassifyLiquidInstance(r);
                         });
        ModernWater::TraceLiquidDrawList(instances.m_data, count);
    }

    WaterClass pendingClass = WaterClass::None;

    for (uint32_t i = 0; i < count; i++) {
        Liquid::CInstance* instance = instances.m_data[i];
        const WaterClass waterClass = armed ? ModernWater::ClassifyLiquidInstance(instance) : WaterClass::None;

        // Before drawing a different class, the class that was drawing has nothing more coming, so its run is
        // finished and can be shaded.
        if (armed && waterClass != pendingClass && pendingClass != WaterClass::None) {
            water.ShadeRun(device, pendingClass);
        }

        if (armed && waterClass != WaterClass::None) {
            water.Tag(device, waterClass);
            pendingClass = waterClass;
        }

        instance->m_material->Render(instance->m_environment, instance->m_geomFactory, instance->m_waveManager, cameraPos, &instance->m_transform, &instance->m_bounds, instance->m_settings);
        instance->m_queued = 0;
    }

    instances.Clear();

    if (armed) {
        if (pendingClass != WaterClass::None) {
            water.ShadeRun(device, pendingClass);
        }

        water.End(device);
    }

    if (pass == 1) {
        ModernWater::SetClientRipplesSuppressed(armed);
    }

    for (uint32_t i = 0; i < limit; i++) {
        if (lights[i].m_flags & 0x1) {
            g_theGxDevicePtr->LightSet(i, lights[i], C3Vector(0.0f, 0.0f, 0.0f));
            g_theGxDevicePtr->LightEnable(i, 1);
        }
    }
}

// VERIFY: 520282aa manual
// OFFSET: 0x005EEB70
void Liquid::CRenderer::Validate() {
}
