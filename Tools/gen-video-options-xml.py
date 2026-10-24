#!/usr/bin/env python3
"""Generates Data/custom.MPQ/Interface/{FrameXML,GlueXML}/VideoOptionsPanels.xml.

The in-game (FrameXML) and login-screen (GlueXML) video options are the same layout with different fonts, dropdown
templates and tooltip calls, and the old hand-kept pair drifted apart. Edit the page specs below, not the XML, then:

    python Tools/gen-video-options-xml.py

Layout model (all numbers are UI units):
  * Every category is a PAGE (plain frame registered with the options frame) holding a fixed title and subtext, and a
    scroll frame under them. The scroll frame keeps the old panel's global name, so every control keeps its old
    global name and GraphicsQualityLevels.lua / VideoOptionsPanels.lua keep finding them.
  * The scroll child is PAGE_W - 38 wide. Boxes stretch to it; their height and every row's offset are computed here,
    so a row never depends on a neighbour's anchor.
  * Each control is a row: checkbox / slider / dropdown plus one grey line of description underneath.
"""

import os
import sys
from xml.sax.saxutils import escape

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Data", "custom.MPQ", "Interface")

# VideoOptionsFrame is resized in VideoOptionsPanels.lua to FRAME_W x FRAME_H; the page is the panel container.
PAGE_W = 525
CHILD_W = PAGE_W - 38

BOX_X = 6            # box inset inside the scroll child
BOX_GAP = 30         # gap between a box's bottom and the next box's top (leaves room for the box title)
BOX_TOP = 24         # first box top (room for its title)
PAD_X = 14           # row inset inside a box
PAD_TOP = 14
PAD_BOTTOM = 10
INDENT = 22          # dependent controls sit this far right of their parent

ROW_CHECK = 38
ROW_SLIDER = 66
ROW_DROPDOWN = 52
SLIDER_INSET = 24    # slider track inset from the box edge (the thumb overhangs the track by half its width)
DROPDOWN_X = 132     # dropdown frame x inside the box; the label column is to its left


class Flavor:
    def __init__(self, name):
        self.name = name
        self.frame = name == "frame"

    def font(self, kind):
        table = {
            "title": ("GameFontNormalLarge", "OptionsFontLarge"),
            "small": ("GameFontNormalSmall", "OptionsFontSmall"),
            "highlight": ("GameFontHighlightSmall", "OptionsFontHighlightSmall"),
            "normal": ("GameFontNormal", "OptionsFont"),
            "left": ("GameFontNormalLeft", "OptionsFontLeft"),
            "extrasmall": ("GameFontHighlightExtraSmallLeftTop", "OptionsFontHighlightSmallLeftTop"),
        }
        return table[kind][0 if self.frame else 1]

    @property
    def dropdown(self):
        return "UIDropDownMenuTemplate" if self.frame else "GlueDropDownMenuTemplate"

    def tooltip_on(self, text_expr):
        if self.frame:
            return ("<OnEnter>\n\tGameTooltip:SetOwner(self, \"ANCHOR_RIGHT\");\n"
                    "\tGameTooltip:SetText(%s, nil, nil, nil, nil, 1);\n</OnEnter>\n"
                    "<OnLeave function=\"GameTooltip_Hide\"/>") % text_expr
        return ("<OnEnter>\n\tOptionsTooltip:SetOwner(self);\n\tOptionsTooltip:SetText(%s);\n</OnEnter>\n"
                "<OnLeave>\n\tOptionsTooltip:Hide();\n</OnLeave>") % text_expr


def q(text):
    return escape(text, {'"': "&quot;"})


class Out:
    def __init__(self):
        self.lines = []
        self.depth = 0

    def add(self, text):
        for line in text.split("\n"):
            self.lines.append(("\t" * self.depth + line) if line.strip() else "")

    def open(self, text):
        self.add(text)
        self.depth += 1

    def close(self, text):
        self.depth -= 1
        self.add(text)

    def block(self, text):
        self.add(text)


# --- row model ----------------------------------------------------------------------------------------------

class Check:
    height = ROW_CHECK

    def __init__(self, name, cvar, desc, restart=False, dep=None, set_func=None, extra="", hidden_on_mac=False):
        self.name, self.cvar, self.desc = name, cvar, desc
        self.restart, self.dep, self.set_func = restart, dep, set_func
        self.extra, self.hidden_on_mac = extra, hidden_on_mac


class Slider:
    height = ROW_SLIDER

    def __init__(self, name, desc, cvar=None, label=None, onload=None, handler="VideoOptionsEffectsPanelSlider_OnValueChanged",
                 dep=None, extra_changed=""):
        self.name, self.desc, self.cvar, self.label = name, desc, cvar, label
        self.onload, self.handler, self.dep, self.extra_changed = onload, handler, dep, extra_changed


class Dropdown:
    height = ROW_DROPDOWN

    def __init__(self, name, label, desc, onload, ident, tooltip):
        self.name, self.label, self.desc, self.onload, self.ident, self.tooltip = name, label, desc, onload, ident, tooltip


class Raw:
    """A hand-placed block inside a box: `build(out, flavor, box, y)` writes it; `height` reserves room."""

    def __init__(self, height, build):
        self.height, self.build = height, build


class Box:
    def __init__(self, name, title, rows, indent_of=None):
        self.name, self.title, self.rows = name, title, rows

    @property
    def height(self):
        return PAD_TOP + sum(row.height for row in self.rows) + PAD_BOTTOM


class Page:
    def __init__(self, page, panel, label_global, sub_global, boxes, first=False, pre=None, flavors=("frame", "glue")):
        self.page, self.panel = page, panel
        self.label_global, self.sub_global = label_global, sub_global
        self.boxes, self.first, self.pre, self.flavors = boxes, first, pre, flavors


def check_desc(text):
    return "\n".join((
        '<Layers><Layer level="ARTWORK"><FontString name="$parentDesc" inherits="VideoOptionsCheckDescFont" text="%s">' % q(text),
        '\t<Size x="420" y="12"/>',
        '\t<Anchors><Anchor point="TOPLEFT" relativePoint="TOPLEFT"><Offset x="25" y="-22"/></Anchor></Anchors>',
        '</FontString></Layer></Layers>'))


def slider_desc(text):
    return "\n".join((
        '<Layers><Layer level="ARTWORK"><FontString name="$parentDesc" inherits="VideoOptionsSliderDescFont" text="%s">' % q(text),
        '\t<Size y="12"/>',
        '\t<Anchors>',
        '\t\t<Anchor point="TOPLEFT" relativePoint="BOTTOMLEFT"><Offset x="0" y="-10"/></Anchor>',
        '\t\t<Anchor point="TOPRIGHT" relativePoint="BOTTOMRIGHT"><Offset x="0" y="-10"/></Anchor>',
        '\t</Anchors>',
        '</FontString></Layer></Layers>'))


# --- emitters -----------------------------------------------------------------------------------------------

def emit_check(o, f, panel, box, row, y, indent):
    x = PAD_X + indent
    o.open('<CheckButton name="$parent%s" inherits="VideoOptionsCheckTemplate">' % row.name)
    o.add('<Anchors><Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor></Anchors>'
          % (box.name, x, -y))
    o.add(check_desc(row.desc))
    o.open("<Scripts>")
    o.open("<OnLoad>")
    flags = ["true" if row.restart else "false"]
    call = 'VideoOptionsCheck_OnLoad(self, "%s", %s%s);' % (row.cvar, flags[0], (", " + row.set_func) if row.set_func else "")
    o.add(call)
    if row.hidden_on_mac:
        o.add("if ( IsMacClient() ) then\n\tself:Hide();\nend")
    if row.dep:
        o.add("BlizzardOptionsPanel_SetupDependentControl(%s%s, self);" % (panel, row.dep))
    if row.extra:
        o.add(row.extra)
    o.close("</OnLoad>")
    o.close("</Scripts>")
    o.close("</CheckButton>")


def emit_slider(o, f, panel, box, row, y, indent):
    x = SLIDER_INSET + indent
    top = y + 18
    o.open('<Slider name="$parent%s" inherits="VideoOptionsSliderTemplate">' % row.name)
    o.open("<Anchors>")
    o.add('<Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor>' % (box.name, x, -top))
    o.add('<Anchor point="TOPRIGHT" relativeTo="$parent%s" relativePoint="TOPRIGHT"><Offset x="%d" y="%d"/></Anchor>' % (box.name, -SLIDER_INSET, -top))
    o.close("</Anchors>")
    o.add(slider_desc(row.desc))
    o.open("<Scripts>")
    o.open("<OnLoad>")
    if row.onload:
        o.add(row.onload.strip("\n"))
    else:
        o.add('VideoOptionsSlider_OnLoad(self, "%s");' % row.cvar)
    if row.dep:
        o.add("BlizzardOptionsPanel_SetupDependentControl(%s%s, self);" % (panel, row.dep))
    o.close("</OnLoad>")
    o.open("<OnValueChanged>")
    o.add("VideoOptionsSlider_UpdateValue(self, value);")
    o.add("%s(self, value);" % row.handler if row.handler else "")
    if row.extra_changed:
        o.add(row.extra_changed)
    o.close("</OnValueChanged>")
    o.close("</Scripts>")
    o.close("</Slider>")


def emit_dropdown(o, f, panel, box, row, y, indent):
    o.open('<Frame name="$parent%s" inherits="%s" id="%d" enableMouse="true">' % (row.name, f.dropdown, row.ident))
    o.add('<Anchors><Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor></Anchors>'
          % (box.name, DROPDOWN_X, -y))
    o.open("<Layers>")
    o.open('<Layer level="BACKGROUND">')
    o.add('<FontString name="$parentLabel" inherits="%s" justifyH="LEFT" text="%s">' % (f.font("small"), q(row.label)))
    o.add('\t<Size x="%d" y="14"/>' % (DROPDOWN_X - PAD_X - 6))
    o.add('\t<Anchors><Anchor point="LEFT" relativePoint="TOPLEFT"><Offset x="%d" y="-17"/></Anchor></Anchors>' % -(DROPDOWN_X - PAD_X))
    o.add("</FontString>")
    o.add('<FontString name="$parentDesc" inherits="VideoOptionsCheckDescFont" text="%s">' % q(row.desc))
    o.add('\t<Anchors><Anchor point="TOPLEFT" relativePoint="TOPLEFT"><Offset x="%d" y="-35"/></Anchor></Anchors>' % -(DROPDOWN_X - PAD_X))
    o.add("</FontString>")
    o.close("</Layer>")
    o.close("</Layers>")
    o.open("<Scripts>")
    o.add("<OnLoad>\n\tself.type = CONTROLTYPE_DROPDOWN;\n\tBlizzardOptionsPanel_RegisterControl(self, self:GetParent());\n\t%s(self);\n</OnLoad>" % row.onload)
    o.block(f.tooltip_on(row.tooltip))
    o.close("</Scripts>")
    o.close("</Frame>")


EMITTERS = {Check: emit_check, Slider: emit_slider, Dropdown: emit_dropdown}


def emit_box(o, f, panel, box, prev, top):
    o.open('<Frame name="$parent%s" inherits="OptionsBoxTemplate">' % box.name)
    o.open("<Anchors>")
    if prev is None:
        o.add('<Anchor point="TOPLEFT"><Offset x="%d" y="%d"/></Anchor>' % (BOX_X, -top))
        o.add('<Anchor point="TOPRIGHT"><Offset x="%d" y="%d"/></Anchor>' % (-BOX_X, -top))
    else:
        o.add('<Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="BOTTOMLEFT"><Offset x="0" y="%d"/></Anchor>' % (prev.name, -BOX_GAP))
        o.add('<Anchor point="TOPRIGHT" relativeTo="$parent%s" relativePoint="BOTTOMRIGHT"><Offset x="0" y="%d"/></Anchor>' % (prev.name, -BOX_GAP))
    o.close("</Anchors>")
    o.add('<Size y="%d"/>' % box.height)
    o.add('<Scripts><OnLoad>VideoOptionsBox_OnLoad(self, "%s");</OnLoad></Scripts>' % q(box.title))
    o.close("</Frame>")

    y = PAD_TOP
    for row in box.rows:
        indent = INDENT if getattr(row, "dep", None) else 0
        if isinstance(row, Raw):
            row.build(o, f, panel, box, y)
        else:
            EMITTERS[type(row)](o, f, panel, box, row, y, indent)
        y += row.height


def emit_page(o, f, page):
    panel = page.panel
    height = BOX_TOP
    for index, box in enumerate(page.boxes):
        height += box.height + (BOX_GAP if index < len(page.boxes) - 1 else 0)
    height += 12

    o.open('<Frame name="%s" parent="VideoOptionsFramePanelContainer" hidden="true">' % page.page)
    o.open("<Layers>")
    o.open('<Layer level="ARTWORK">')
    o.add('<FontString name="$parentTitle" text="%s" inherits="%s" justifyH="LEFT" justifyV="TOP">\n'
          '\t<Anchors><Anchor point="TOPLEFT"><Offset x="14" y="-12"/></Anchor></Anchors>\n</FontString>'
          % (page.label_global, f.font("title")))
    o.add('<FontString name="$parentSubText" text="%s" inherits="%s" nonspacewrap="true" maxLines="2" justifyH="LEFT" justifyV="TOP">\n'
          '\t<Size y="26"/>\n'
          '\t<Anchors>\n'
          '\t\t<Anchor point="TOPLEFT"><Offset x="14" y="-36"/></Anchor>\n'
          '\t\t<Anchor point="TOPRIGHT"><Offset x="-14" y="-36"/></Anchor>\n'
          '\t</Anchors>\n</FontString>' % (page.sub_global, f.font("highlight")))
    o.close("</Layer>")
    o.close("</Layers>")
    o.open("<Frames>")
    o.open('<ScrollFrame name="%s" inherits="UIPanelScrollFrameTemplate">' % panel)
    o.add('<Anchors>\n\t<Anchor point="TOPLEFT"><Offset x="8" y="-66"/></Anchor>\n\t<Anchor point="BOTTOMRIGHT"><Offset x="-30" y="8"/></Anchor>\n</Anchors>')
    o.add("<Scripts>\n\t<OnLoad>\n\t\tself.scrollBarHideable = 1;\n\t\tScrollFrame_OnLoad(self);\n\t</OnLoad>\n</Scripts>")
    o.open("<ScrollChild>")
    o.add("<!-- Unnamed on purpose: $parent skips unnamed frames, so every control keeps its %s global name. -->" % panel)
    o.open("<Frame>")
    o.add('<Size x="%d" y="%d"/>' % (CHILD_W, height))
    o.open("<Frames>")
    if page.pre:
        page.pre(o, f, panel)
    prev = None
    top = BOX_TOP
    for box in page.boxes:
        emit_box(o, f, panel, box, prev, top)
        prev = box
    o.close("</Frames>")
    o.close("</Frame>")
    o.close("</ScrollChild>")
    o.close("</ScrollFrame>")
    o.close("</Frames>")
    o.add("<Scripts>\n\t<OnLoad>\n\t\t%s_OnLoad(self);\n\t</OnLoad>\n\t<OnShow>\n\t\tVideoOptionsPage_OnShow(self);\n\t</OnShow>\n</Scripts>" % panel)
    o.close("</Frame>")


# --- hand-placed pieces ---------------------------------------------------------------------------------------

GAMMA_SLIDER_TOP = ROW_CHECK + 18
GAMMA_BAR_TOP = GAMMA_SLIDER_TOP + 45
GAMMA_HEIGHT = GAMMA_BAR_TOP + 26 + 4


def build_gamma(o, f, panel, box, y):
    # Desktop gamma toggle, gamma slider and the 21-step greyscale bar used to judge it.
    o.open('<CheckButton name="$parentDesktopGamma" inherits="VideoOptionsCheckTemplate">')
    o.add('<Anchors><Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor></Anchors>' % (box.name, PAD_X, -y))
    o.add(check_desc("Use your desktop's brightness curve. Always on while windowed."))
    o.open("<Scripts>")
    o.add("<OnLoad>\n\tVideoOptionsCheck_OnLoad(self, \"desktopGamma\", false, VideoOptionsResolutionPanel_SetGamma);\n</OnLoad>")
    o.add("<OnClick>\n"
          "\tBlizzardOptionsPanel_CheckButton_OnClick(self);\n"
          "\tif ( self:GetParent():IsShown() ) then\n"
          "\t\tlocal value;\n"
          "\t\tif ( self:GetChecked() ) then\n"
          "\t\t\tvalue = 1;\n"
          "\t\telse\n"
          "\t\t\tvalue = 0;\n"
          "\t\t\tSetGamma(VideoOptionsResolutionPanelGammaSlider:GetValue());\n"
          "\t\tend\n"
          "\t\tBlizzardOptionsPanel_SetCVarSafe(self.cvar, value);\n"
          "\tend\n"
          "\tVideoOptionsFrameApply:Enable();\n"
          "</OnClick>")
    o.close("</Scripts>")
    o.close("</CheckButton>")

    top = y + GAMMA_SLIDER_TOP
    o.open('<Slider name="$parentGammaSlider" inherits="VideoOptionsSliderTemplate">')
    o.add('<Anchors>\n\t<Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor>\n'
          '\t<Anchor point="TOPRIGHT" relativeTo="$parent%s" relativePoint="TOPRIGHT"><Offset x="%d" y="%d"/></Anchor>\n</Anchors>'
          % (box.name, SLIDER_INSET, -top, box.name, -SLIDER_INSET, -top))
    o.add(slider_desc("Screen brightness. The grey bar below should show every step."))
    o.open("<Scripts>")
    o.add("<OnLoad>\n"
          "\tself.type = CONTROLTYPE_SLIDER;\n"
          "\tself.label = \"gamma\";\n"
          "\tVideoOptionsSlider_Setup(self);\n"
          "\tself.SetDisplayValue = self.SetValue;\n"
          "\tself.SetValue = function (self, value)\n"
          "\t\tself:SetDisplayValue(value);\n"
          "\t\tSetGamma(value);\n"
          "\tend\n"
          "\tself.GetCurrentValue = function (self) return GetGamma(); end;\n"
          "\tBlizzardOptionsPanel_RegisterControl(self, self:GetParent());\n"
          "</OnLoad>")
    o.add("<OnValueChanged>\n"
          "\tVideoOptionsSlider_UpdateValue(self, value);\n"
          "\tself.newValue = value;\n"
          "\tif ( self:GetParent():IsShown() ) then\n"
          "\t\tSetGamma(value);\n"
          "\tend\n"
          "\tVideoOptionsFrameApply:Enable();\n"
          "</OnValueChanged>")
    o.close("</Scripts>")
    o.close("</Slider>")

    o.open('<Frame name="$parentGrayScaleBar">')
    o.add('<Size y="26"/>')
    o.add('<Anchors>\n\t<Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor>\n'
          '\t<Anchor point="TOPRIGHT" relativeTo="$parent%s" relativePoint="TOPRIGHT"><Offset x="%d" y="%d"/></Anchor>\n</Anchors>'
          % (box.name, SLIDER_INSET, -(y + GAMMA_BAR_TOP), box.name, -SLIDER_INSET, -(y + GAMMA_BAR_TOP)))
    o.add('<Layers><Layer level="ARTWORK"><Texture file="Interface\\OptionsFrame\\21stepgrayscale">')
    o.add('\t<Anchors><Anchor point="TOPLEFT"/><Anchor point="BOTTOMRIGHT"/></Anchors>')
    o.add('</Texture></Layer></Layers>')
    o.close("</Frame>")




def preset_rows():
    def build(o, f, panel, box, y):
        o.open('<Frame name="$parentPresetBody">')
        o.add('<Anchors>\n\t<Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor>\n'
              '\t<Anchor point="TOPRIGHT" relativeTo="$parent%s" relativePoint="TOPRIGHT"><Offset x="%d" y="%d"/></Anchor>\n</Anchors>'
              % (box.name, PAD_X, -y, box.name, -PAD_X, -y))
        o.add('<Size y="%d"/>' % PRESET_HEIGHT)
        o.open("<Layers>")
        o.open('<Layer level="ARTWORK">')
        o.add('<FontString name="VideoOptionsEffectsPanelQualityLabel" inherits="%s" justifyH="LEFT" text="LOW">\n'
              '\t<Anchors><Anchor point="TOPLEFT"/></Anchors>\n</FontString>' % f.font("normal"))
        o.add('<FontString name="VideoOptionsEffectsPanelQualitySubText" inherits="%s" justifyH="LEFT" justifyV="TOP" text="">\n'
              '\t<Size y="26"/>\n'
              '\t<Anchors>\n'
              '\t\t<Anchor point="TOPLEFT" relativeTo="VideoOptionsEffectsPanelQualityLabel" relativePoint="BOTTOMLEFT"><Offset x="0" y="-4"/></Anchor>\n'
              '\t\t<Anchor point="RIGHT"/>\n'
              '\t</Anchors>\n</FontString>' % f.font("highlight"))
        o.close("</Layer>")
        o.close("</Layers>")
        o.close("</Frame>")
        o.open('<Slider name="$parentQualitySlider" inherits="OptionsSliderTemplate">')
        o.add('<Anchors>\n\t<Anchor point="TOPLEFT" relativeTo="$parent%s" relativePoint="TOPLEFT"><Offset x="%d" y="%d"/></Anchor>\n'
              '\t<Anchor point="TOPRIGHT" relativeTo="$parent%s" relativePoint="TOPRIGHT"><Offset x="%d" y="%d"/></Anchor>\n</Anchors>'
              % (box.name, SLIDER_INSET, -(y + 50), box.name, -SLIDER_INSET, -(y + 50)))
        o.add('<Layers><Layer level="ARTWORK">\n'
              '\t<FontString name="$parentCustom" inherits="%s" text="CUSTOM">\n'
              '\t\t<Anchors><Anchor point="TOPRIGHT" relativePoint="BOTTOMRIGHT"><Offset x="4" y="3"/></Anchor></Anchors>\n'
              '\t</FontString>\n</Layer></Layers>' % f.font("highlight"))
        o.open("<Scripts>")
        o.add("<OnLoad>\n"
              "\tself.type = CONTROLTYPE_SLIDER;\n"
              "\tself.label = \"quality\";\n"
              "\tVideoOptionsQualitySlider_Setup(self);\n"
              "\tBlizzardOptionsPanel_RegisterControl(self, self:GetParent());\n"
              "</OnLoad>")
        o.add("<OnValueChanged>\n"
              "\tself.newValue = value;\n"
              "\tif ( self:GetParent():IsShown() ) then\n"
              "\t\tVideoOptionsEffectsPanel_SetVideoQuality(value);\n"
              "\tend\n"
              "</OnValueChanged>")
        o.close("</Scripts>")
        o.close("</Slider>")
    return [Raw(PRESET_HEIGHT, build)]


PRESET_HEIGHT = 88


# --- the pages --------------------------------------------------------------------------------------------

def pages(flavor):
    frame = flavor.frame

    display_boxes = [
        Box("ScreenBox", "Screen", [
            Dropdown("ResolutionDropDown", "Resolution", "Size of the game image. Higher is sharper but slower.",
                     "VideoOptionsResolutionPanelResolutionDropDown_OnLoad", 1, "OPTION_TOOLTIP_USE_RESOLUTION"),
            Dropdown("RefreshDropDown", "Refresh Rate", "How many times a second the screen redraws. Match your monitor.",
                     "VideoOptionsResolutionPanelRefreshDropDown_OnLoad", 2, "OPTION_TOOLTIP_USE_REFRESH"),
            Check("Windowed", "gxWindow", "Run in a window instead of full screen.", restart=True,
                  set_func="VideoOptionsResolutionPanel_SetWindowed"),
            Check("Maximized", "gxMaximize", "Stretch the window to fill the desktop.", restart=True, dep="Windowed"),
            Check("DisableResize", "windowResizeLock", "Stop the window from being resized by dragging its edges.",
                  dep="Windowed", hidden_on_mac=True),
            Check("VSync", "gxVSync", "Wait for the monitor's refresh to prevent screen tearing.", restart=True),
            Check("TripleBuffer", "gxTripleBuffer", "Smooths vertical sync at the cost of a little video memory.",
                  restart=True, dep="VSync"),
        ]),
        Box("BrightnessBox", "Brightness", [Raw(GAMMA_HEIGHT, build_gamma)]),
    ]
    if frame:
        display_boxes.append(Box("InterfaceBox", "Interface", [
            Check("UseUIScale", "useUiScale", "Use a custom interface size instead of the automatic one.", restart=True),
            Slider("UIScaleSlider", "Smaller shows more of the world. Larger is easier to read.", dep="UseUIScale",
                   onload=(
                       "self.type = CONTROLTYPE_SLIDER;\n"
                       "self.cvar = \"uiscale\";\n"
                       "VideoOptionsSlider_Setup(self);\n"
                       "self.SetDisplayValue = self.SetValue;\n"
                       "self.SetValue = function (self, value)\n"
                       "\tself:SetDisplayValue(value);\n"
                       "\tBlizzardOptionsPanel_SetCVarSafe(self.cvar, value);\n"
                       "end\n"
                       "BlizzardOptionsPanel_RegisterControl(self, self:GetParent());"),
                   handler=None, extra_changed="self.newValue = value;\nVideoOptionsFrameApply:Enable();"),
        ]))

    graphics_boxes = [
        Box("PresetBox", "Quality Preset", preset_rows()),
        Box("WorldBox", "World Detail", [
            Slider("ViewDistance", "How far you can see, in yards. Lower it to gain frame rate.", cvar="farclip"),
            Slider("EnvironmentDetail", "How far trees, buildings and other scenery are drawn.", cvar="environmentDetail",
                   onload=_pct("environmentDetail")),
            Slider("TerrainDetail", "Smooths the seams between ground textures. Applied at the next login.", label="TerrainMip",
                   onload=_terrain_onload()),
            Slider("WeatherIntensity", "How much rain, snow and dust fills the air.", cvar="weatherDensity"),
        ]),
        Box("TextureBox", "Textures", [
            Slider("TextureResolution", "Sharpness of world textures. High uses more video memory.", cvar="BaseMip",
                   onload=_basemip_onload()),
            Slider("TextureFiltering", "Cleans up textures seen at a slant, like the ground ahead. Needs a restart.",
                   cvar="textureFilteringMode"),
            Slider("PlayerTexture", "Detail of character and creature textures. Needs a restart.", cvar="componentTextureLevel"),
        ]),
        Box("EffectsBox", "Effects", [
            Slider("ParticleDensity", "How many particles spells and effects can spawn.", cvar="particleDensity",
                   onload=_pct("particleDensity")),
            Check("FullScreenGlow", "ffxGlow", "A soft bloom around bright lights.",
                  set_func="VideoOptionsEffectsPanel_UpdateVideoQuality"),
            Check("DeathEffect", "ffxDeath", "Drain the colour from the screen while you are a ghost.",
                  set_func="VideoOptionsEffectsPanel_UpdateVideoQuality"),
            Check("ProjectedTextures", "projectedTextures", "Draw spell targeting and area effects onto the ground.",
                  set_func="VideoOptionsEffectsPanel_UpdateVideoQuality"),
        ]),
        Box("AntiAliasBox", "Anti-Aliasing", [
            Dropdown("MultiSampleDropDown", "Multisampling", "Hardware edge smoothing. Best quality, highest cost.",
                     "VideoOptionsEffectsPanelMultiSampleDropDown_OnLoad", 4, "OPTION_TOOLTIP_MULTISAMPLING"),
            Dropdown("AntiAliasDropDown", "Anti-Aliasing", "Post-process smoothing that keeps text sharp. Skipped with multisampling.",
                     "VideoOptionsEffectsPanelAntiAliasDropDown_OnLoad", 7, "OPTION_TOOLTIP_ANTI_ALIASING"),
            Dropdown("FxaaQualityDropDown", "FXAA Quality", "How hard FXAA smooths. Higher can soften fine detail.",
                     "VideoOptionsEffectsPanelFxaaQualityDropDown_OnLoad", 8, "OPTION_TOOLTIP_FXAA_QUALITY"),
        ]),
    ]

    shadow_handler = "VideoOptionsShadowsPanelSlider_OnValueChanged"
    shadows_boxes = [
        Box("ShadowBox", "Shadows", [
            Slider("ShadowQuality", "Shadow method and how often shadows update. Lower it for frame rate.", label="ShadowQuality",
                   handler=shadow_handler, onload=_shadow_onload("ShadowQuality", "VideoOptionsShadowsPanel_SetShadowQuality",
                                                                 "VideoOptionsShadowsPanel_GetShadowQuality")),
            Slider("ShadowResolution", "Sharpness of shadow edges. Does not change how often they update.", label="ShadowResolution",
                   handler=shadow_handler, onload=_shadow_onload("ShadowResolution", "VideoOptionsShadowsPanel_SetShadowResolution",
                                                                 "VideoOptionsShadowsPanel_GetShadowResolution")),
            Check("CascadedShadows", "cascadedShadows", "At the top shadow step, use cascaded maps. Off falls back to tiled shadows."),
        ]),
    ]

    light_handler = "VideoOptionsLightingPanelSlider_OnValueChanged"
    lighting_boxes = [
        Box("AmbientBox", "Ambient Occlusion", [
            Check("Ssao", "ssao", "Darkens creases and contact points. Not applied with multisampling."),
            Slider("SsaoStrength", "How dark the occluded areas get.", cvar="ssaoStrength", dep="Ssao", handler=light_handler),
            Slider("SsaoSpread", "How far the shading reaches from a crease. Low is tight contact shadows.", cvar="ssaoSpread",
                   dep="Ssao", handler=light_handler),
            Slider("SsaoDistance", "How far from the camera it applies, in yards. Lower is cheaper.", cvar="ssaoDistance",
                   dep="Ssao", handler=light_handler),
        ]),
        Box("SunBox", "Sun", [
            Check("SunGlow", "sunGlow", "A soft glow around the sun, hidden behind trees, buildings and mountains."),
            Slider("SunGlowStrength", "How bright the glow is.", cvar="sunGlowStrength", dep="SunGlow", handler=light_handler),
        ]),
        Box("AtmosphereBox", "Atmosphere", [
            Check("DepthFog", "depthFog", "Distance haze that hides the edge of the view and fades far ridges into the sky."),
        ]),
    ]

    foliage_boxes = [
        Box("ClutterBox", "Clutter", [
            Slider("ClutterDensity", "How much grass and small plants grow in each area. High costs frame rate.",
                   cvar="groundEffectDensity"),
            Slider("ClutterRadius", "How far grass and clutter is drawn, in yards.", cvar="groundEffectDist"),
        ]),
        Box("WindBox", "Wind", [
            Slider("WindSway", "How far the tip of a blade leans at full strength.", cvar="grassWindSway"),
            Slider("WindFlutter", "Extra shimmer per blade, so a field moves as blades and not one sheet.",
                   cvar="grassWindFlutter"),
            Slider("WindSpeed", "How fast the wind travels.", cvar="grassWindSpeed"),
        ]),
        Box("PartingBox", "Grass Parting", [
            Check("Parting", "grassParting", "Let grass bend away from nearby characters."),
            Slider("PartingStrength", "How far blades lean away from a character.", cvar="grassPartingStrength", dep="Parting"),
            Slider("PartingRadius", "Size of the pushed area, compared to the character.", cvar="grassPartingRadiusScale",
                   dep="Parting"),
        ]),
    ]

    gameplay_boxes = [
        Box("HighlightBox", "Highlights", [
            Check("UnitOutline", "unitOutline", "Outline the unit under your cursor and your target."),
            Check("LootBeam", "lootBeam", "Raise a beam of light over corpses you can still loot, tinted by the best item."),
        ]),
        Box("CursorBox", "Mouse Cursor", [
            Check("HardwareCursor", "gxCursor", "Draw the pointer on the GPU so it stays smooth at any frame rate.", restart=True),
            Check("FixInputLag", "gxFixLag", "Sync the GPU every frame to cut pointer delay, at a frame rate cost.", restart=True),
        ]),
        Box("DiagnosticsBox", "Diagnostics", [
            Check("Show", "showPerformance", "A movable overlay with frame rate, frame time, draw calls and CPU use."),
        ]),
    ]

    return [
        Page("VideoOptionsResolutionPage", "VideoOptionsResolutionPanel", "VIDEOOPT_DISPLAY", "VIDEOOPT_DISPLAY_SUBTEXT", display_boxes),
        Page("VideoOptionsEffectsPage", "VideoOptionsEffectsPanel", "VIDEOOPT_GRAPHICS", "VIDEOOPT_GRAPHICS_SUBTEXT", graphics_boxes),
        Page("VideoOptionsShadowsPage", "VideoOptionsShadowsPanel", "VIDEOOPT_SHADOWS", "VIDEOOPT_SHADOWS_SUBTEXT", shadows_boxes),
        Page("VideoOptionsLightingPage", "VideoOptionsLightingPanel", "VIDEOOPT_LIGHTING", "VIDEOOPT_LIGHTING_SUBTEXT", lighting_boxes),
        Page("VideoOptionsPhysicsPage", "VideoOptionsPhysicsPanel", "VIDEOOPT_FOLIAGE", "VIDEOOPT_FOLIAGE_SUBTEXT", foliage_boxes),
        Page("VideoOptionsGameplayPage", "VideoOptionsGameplayPanel", "VIDEOOPT_GAMEPLAY", "VIDEOOPT_GAMEPLAY_SUBTEXT", gameplay_boxes),
    ]


def _pct(cvar):
    return ('VideoOptionsSlider_OnLoad(self, "%s");\n'
            "self.GetCurrentValue = VideoOptionsEffectsPanel_GetPercentCVar;" % cvar)


def _terrain_onload():
    return ("self.type = CONTROLTYPE_SLIDER;\n"
            "self.label = \"TerrainMip\";\n"
            "VideoOptionsSlider_Setup(self);\n"
            "self.SetDisplayValue = self.SetValue;\n"
            "self.SetValue = function (self, value)\n"
            "\tself:SetDisplayValue(value);\n"
            "\tSetTerrainMip(value);\n"
            "end\n"
            "self.GetCurrentValue = function (self) return GetTerrainMip(); end;\n"
            "BlizzardOptionsPanel_RegisterControl(self, self:GetParent());")


def _basemip_onload():
    return ("self.type = CONTROLTYPE_SLIDER;\n"
            "self.cvar = \"BaseMip\";\n"
            "VideoOptionsSlider_Setup(self);\n"
            "self.SetDisplayValue = self.SetValue;\n"
            "self.SetValue = function (self, value)\n"
            "\tself:SetDisplayValue(value);\n"
            "\tlocal min, max = self:GetMinMaxValues();\n"
            "\tBlizzardOptionsPanel_SetCVarSafe(self.cvar, max - value + min); --With this cvar, lower is actually better quality.\n"
            "end\n"
            "self.GetCurrentValue = function (self)\n"
            "\tlocal value = BlizzardOptionsPanel_GetCVarSafe(self.cvar);\n"
            "\tlocal min, max = self:GetMinMaxValues();\n"
            "\treturn min - value + max;\n"
            "end\n"
            "BlizzardOptionsPanel_RegisterControl(self, self:GetParent());")


def _shadow_onload(label, setter, getter):
    return ("self.type = CONTROLTYPE_SLIDER;\n"
            "self.label = \"%s\";\n"
            "VideoOptionsSlider_Setup(self);\n"
            "self.SetDisplayValue = self.SetValue;\n"
            "self.SetValue = function (self, value)\n"
            "\tself:SetDisplayValue(value);\n"
            "\t%s(value);\n"
            "end\n"
            "self.GetCurrentValue = %s;\n"
            "BlizzardOptionsPanel_RegisterControl(self, self:GetParent());" % (label, setter, getter))


# --- shared templates -------------------------------------------------------------------------------------------

def templates(o, f):
    o.add("""<Font name="VideoOptionsCheckDescFont" inherits="%(h)s" justifyH="LEFT" justifyV="TOP" virtual="true">
	<Color r="0.62" g="0.62" b="0.62"/>
</Font>
<Font name="VideoOptionsSliderDescFont" inherits="%(h)s" justifyH="LEFT" justifyV="TOP" virtual="true">
	<Color r="0.62" g="0.62" b="0.62"/>
</Font>

<CheckButton name="VideoOptionsSmallCheckButtonTemplate" inherits="OptionsSmallCheckButtonTemplate" virtual="true">
	<Scripts>
		<OnClick>
			if ( self:GetChecked() ) then
				PlaySound("igMainMenuOptionCheckBoxOn");
			else
				PlaySound("igMainMenuOptionCheckBoxOff");
			end
			BlizzardOptionsPanel_CheckButton_OnClick(self);
			VideoOptionsFrameApply:Enable();
		</OnClick>
	</Scripts>
</CheckButton>

<!-- A checkbox row: the box, its label (set by the panel from its options table) and one grey description line
     under the label. The click area covers the whole row so the description is clickable too. -->
<CheckButton name="VideoOptionsCheckTemplate" inherits="VideoOptionsSmallCheckButtonTemplate" virtual="true">
	<Size x="24" y="24"/>
	<HitRectInsets>
		<AbsInset left="0" right="-380" top="0" bottom="-12"/>
	</HitRectInsets>
</CheckButton>

<!-- A slider row: caption above the track at the left, current value at the right, description underneath. -->
<Slider name="VideoOptionsSliderTemplate" inherits="OptionsSliderTemplate" virtual="true">
	<Layers>
		<Layer level="ARTWORK">
			<FontString name="$parentValue" inherits="%(h)s" justifyH="RIGHT">
				<Anchors>
					<Anchor point="BOTTOMRIGHT" relativePoint="TOPRIGHT">
						<Offset x="0" y="4"/>
					</Anchor>
				</Anchors>
			</FontString>
		</Layer>
	</Layers>
</Slider>""" % {"h": f.font("highlight")})
    o.add("")


STEREO_XML = r'''
<Frame name="VideoOptionsStereoPanel" parent="VideoOptionsFramePanelContainer" hidden="true">
	<Layers>
		<Layer level="ARTWORK">
			<FontString name="$parentTitle" text="STEREO_VIDEO_LABEL" inherits="GameFontNormalLarge" justifyH="LEFT" justifyV="TOP">
				<Anchors>
					<Anchor point="TOPLEFT">
						<Offset>
							<AbsDimension x="16" y="-16"/>
						</Offset>
					</Anchor>
				</Anchors>
			</FontString>
			<FontString name="$parentSubText" text="STEREO_VIDEO_SUBTEXT" inherits="GameFontHighlightSmall" nonspacewrap="true" maxLines="3" justifyH="LEFT" justifyV="TOP">
				<Size>
					<AbsDimension y="32" x="0"/>
				</Size>
				<Anchors>
					<Anchor point="TOPLEFT" relativeTo="$parentTitle" relativePoint="BOTTOMLEFT">
						<Offset>
							<AbsDimension x="0" y="-8"/>
						</Offset>
					</Anchor>
					<Anchor point="RIGHT">
						<Offset>
							<AbsDimension x="-32" y="0"/>
						</Offset>
					</Anchor>
				</Anchors>
			</FontString>
		</Layer>
	</Layers>
	<Scripts>
		<OnLoad>
			VideoOptionsStereoPanel_OnLoad(self);
		</OnLoad>
	</Scripts>
	<Frames>
		<CheckButton name="$parentEnabled" inherits="VideoOptionsSmallCheckButtonTemplate">
			<Anchors>
				<Anchor point="TOP">
					<Offset x="-125" y="-92"/>
				</Anchor>
			</Anchors>
			<Scripts>
				<OnLoad>
					self.type = CONTROLTYPE_CHECKBOX;
					self.cvar = "gxStereoEnabled";
					self.restart = true;
					BlizzardOptionsPanel_RegisterControl(self, self:GetParent());
				</OnLoad>
			</Scripts>
		</CheckButton>
		<CheckButton name="$parentHardwareCursor" inherits="VideoOptionsSmallCheckButtonTemplate">
			<Anchors>
				<Anchor point="LEFT" relativeTo="$parentEnabled" relativePoint="RIGHT">
					<Offset>
						<AbsDimension x="135" y="0"/>
					</Offset>
				</Anchor>
			</Anchors>
			<Scripts>
				<OnLoad>
					self.type = CONTROLTYPE_CHECKBOX;
					self.cvar = "gxCursor";
					self.restart = true;
					BlizzardOptionsPanel_RegisterControl(self, self:GetParent());
				</OnLoad>
			</Scripts>
		</CheckButton>
		<Slider name="$parentConvergence" inherits="OptionsSliderTemplate">
			<Size x="270" y="17"/>
			<Anchors>
				<Anchor point="TOP" relativePoint="TOP">
					<Offset x="0" y="-150"/>
				</Anchor>
			</Anchors>
			<Scripts>
				<OnLoad>
					local name = self:GetName();
					self.type = CONTROLTYPE_SLIDER;
					self.cvar = "gxStereoConvergence";
					self.SetDisplayValue = self.SetValue;
					self.SetValue = function (self, value)
						self:SetDisplayValue(value);
						BlizzardOptionsPanel_SetCVarSafe(self.cvar, value);
					end
					_G[name.."Text"]:SetFontObject("GameFontNormalSmall");
					BlizzardOptionsPanel_RegisterControl(self, self:GetParent());
					BlizzardOptionsPanel_SetupDependentControl(VideoOptionsStereoPanelEnabled, self);
				</OnLoad>
				<OnValueChanged>
					self.newValue = self:GetValue();
					BlizzardOptionsPanel_SetCVarSafe(self.cvar, self:GetValue());
					VideoOptionsFrameApply:Enable();
				</OnValueChanged>
			</Scripts>
		</Slider>
		<Slider name="$parentEyeSeparation" inherits="OptionsSliderTemplate">
			<Size x="270" y="17"/>
			<Anchors>
				<Anchor point="TOPLEFT" relativeTo="$parentConvergence" relativePoint="BOTTOMLEFT">
					<Offset x="0" y="-70"/>
				</Anchor>
			</Anchors>
			<Scripts>
				<OnLoad>
					local name = self:GetName();
					self.type = CONTROLTYPE_SLIDER;
					self.cvar = "gxStereoSeparation";
					self.SetDisplayValue = self.SetValue;
					self.SetValue = function (self, value)
						self:SetDisplayValue(value);
						BlizzardOptionsPanel_SetCVarSafe(self.cvar, value);
					end
					_G[name.."Text"]:SetFontObject("GameFontNormalSmall");
					BlizzardOptionsPanel_RegisterControl(self, self:GetParent());
					BlizzardOptionsPanel_SetupDependentControl(VideoOptionsStereoPanelEnabled, self);
				</OnLoad>
				<OnValueChanged>
					self.newValue = self:GetValue();
					BlizzardOptionsPanel_SetCVarSafe(self.cvar, self:GetValue());
					VideoOptionsFrameApply:Enable();
				</OnValueChanged>
			</Scripts>
		</Slider>
	</Frames>
</Frame>
'''


def stereo(o, f):
    text = STEREO_XML.strip("\n")
    if not f.frame:
        for old, new in (("GameFontNormalLarge", "OptionsFontLarge"), ("GameFontHighlightSmall", "OptionsFontHighlightSmall"),
                         ("GameFontNormalSmall", "OptionsFontSmall")):
            text = text.replace(old, new)
    o.add(text)


def generate(flavor_name):
    f = Flavor(flavor_name)
    o = Out()
    o.add('<Ui xmlns="http://www.blizzard.com/wow/ui/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.blizzard.com/wow/ui/')
    o.add("..\\FrameXML\\UI.xsd\">")
    o.depth = 1
    o.add("<!-- Generated by Tools/gen-video-options-xml.py. Edit the generator, not this file; it writes the FrameXML and")
    o.add("     GlueXML copies together so they cannot drift. -->")
    o.add("")
    o.add('<Script file="%sGraphicsQualityLevels.lua"/>' % ("" if f.frame else "..\\FrameXML\\"))
    o.add('<Script file="OptionsPanelTemplates.lua"/>' if f.frame else '<Script file="OptionsPanelTemplates.lua"/>')
    o.add('<Script file="VideoOptionsPanels.lua"/>')
    o.add("")
    templates(o, f)
    for page in pages(f):
        emit_page(o, f, page)
        o.add("")
    stereo(o, f)
    o.depth = 0
    o.add("</Ui>")
    return "\n".join(o.lines) + "\n"


def main():
    for name, folder in (("frame", "FrameXML"), ("glue", "GlueXML")):
        path = os.path.join(ROOT, folder, "VideoOptionsPanels.xml")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(generate(name))
        print("wrote", os.path.normpath(path))


if __name__ == "__main__":
    sys.exit(main())
