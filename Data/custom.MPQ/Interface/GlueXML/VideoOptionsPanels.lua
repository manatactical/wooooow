-- if you change something here you probably want to change the frame version too

local OPTIONS_FARCLIP_MIN = 177;
local OPTIONS_FARCLIP_MAX = 1277;

local VIDEO_OPTIONS_CUSTOM_QUALITY = 6;

local VIDEO_OPTIONS_COMPARISON_EPSILON = 0.000001;


-- [[ Generic Video Options Panel ]] --

function VideoOptionsPanel_Okay (self)
	for _, control in next, self.controls do
		if ( control.newValue ) then
			if ( control.value ~= control.newValue ) then
				if ( control.gameRestart ) then
					VideoOptionsFrame.gameRestart = true;
				end
				if ( control.restart ) then
					VideoOptionsFrame.gxRestart = true;
				end
				control:SetValue(control.newValue);
				control.value = control.newValue;
				control.newValue = nil;
			end
		elseif ( control.value ) then
			control:SetValue(control.value);
		end
	end
end

function VideoOptionsPanel_Cancel (self)
	for _, control in next, self.controls do
		if ( control.newValue ) then
			if ( control.value and control.value ~= control.newValue ) then
				if ( control.restart ) then
					VideoOptionsFrame.gxRestart = true;
				end
				-- we need to force-set the value here just in case the control was doing dynamic updating
				control:SetValue(control.value);
				control.newValue = nil;
			end
		elseif ( control.value ) then
			control:SetValue(control.value);
		end
	end
end

function VideoOptionsPanel_Default (self)
	for _, control in next, self.controls do
		if ( control.defaultValue and control.value ~= control.defaultValue ) then
			if ( control.restart ) then
				VideoOptionsFrame.gxRestart = true;
			end
			control:SetValue(control.defaultValue);
			control.newValue = nil;
		end
	end
end

function VideoOptionsPanel_OnLoad (self, okay, cancel, default, refresh)
	okay = okay or VideoOptionsPanel_Okay;
	cancel = cancel or VideoOptionsPanel_Cancel;
	default = default or VideoOptionsPanel_Default;
	refresh = refresh or BlizzardOptionsPanel_Refresh;
	BlizzardOptionsPanel_OnLoad(self, okay, cancel, default, refresh);

	OptionsFrame_AddCategory(VideoOptionsFrame, self);
end

-- [[ Resolution Panel ]] --

-- Anti-aliasing is not in the original binary. It is a post-process pass over the finished frame, not one
-- of the device's multisample formats, so it is a plain cvar the engine module reads rather than a format
-- offered by the driver.
ANTI_ALIASING = "Anti-Aliasing";
ANTI_ALIASING_OFF = "Off";
ANTI_ALIASING_FXAA = "FXAA";
OPTION_TOOLTIP_ANTI_ALIASING = "Smooths the stair-stepped edges of the 3D view with a post-process pass over the finished frame. The interface, text and icons are never filtered, so they stay sharp. Has no effect while Multisample above is anything other than 1x, where the driver has already smoothed the frame itself, and costs some frame rate.";

-- The FXAA quality presets. The pass itself is identical at every level; what changes is how eagerly a
-- gradient counts as an edge. Low leaves faint gradients alone, Ultra filters them too.
FXAA_QUALITY = "FXAA Quality";
FXAA_QUALITY_LOW = "Low";
FXAA_QUALITY_MEDIUM = "Medium";
FXAA_QUALITY_HIGH = "High";
FXAA_QUALITY_ULTRA = "Ultra";
OPTION_TOOLTIP_FXAA_QUALITY = "How much FXAA smooths. Low only touches strong, obvious silhouettes and leaves everything else exactly as it was drawn; Ultra also smooths faint gradients such as the shimmer on thin geometry and foliage, which costs some detail in busy textures. Medium and High are the standard settings.";

ResolutionPanelOptions = {
	gxVSync = { text = "VERTICAL_SYNC" },
	gxTripleBuffer = { text = "TRIPLE_BUFFER" },
	gxWindow = { text = "WINDOWED_MODE" },
	gxMaximize = { text = "WINDOWED_MAXIMIZED" },
	antiAlias = { text = "ANTI_ALIASING", tooltip = OPTION_TOOLTIP_ANTI_ALIASING },
	fxaaQuality = { text = "FXAA_QUALITY", tooltip = OPTION_TOOLTIP_FXAA_QUALITY },
	windowResizeLock = { text = "WINDOW_LOCK" },
	desktopGamma = { text = "DESKTOP_GAMMA" },
	gamma = { text = "GAMMA", minValue = -.5, maxValue = .5, valueStep = .1 },
}

function VideoOptionsResolutionPanel_Default (self)
	RestoreVideoResolutionDefaults();
	for _, control in next, self.controls do
		control.newValue = nil;
	end
end

function VideoOptionsResolutionPanel_Refresh (self)
	BlizzardOptionsPanel_Refresh(self);
	VideoOptionsResolutionPanel_RefreshGammaControls();
end

function VideoOptionsResolutionPanel_OnLoad (self)
	self.name = RESOLUTION_LABEL;
	self.options = ResolutionPanelOptions;

	-- The panel is a scroll frame now, so the controls sit in a scroll child and register themselves
	-- against it (self:GetParent() inside each dropdown's OnLoad). Adopt that list here, after the
	-- child's frames are created, or the panel would find no controls and never initialize them.
	local child = self:GetScrollChild();
	if ( child and child.controls ) then
		self.controls = child.controls;
	end

	VideoOptionsPanel_OnLoad(self, nil, nil, VideoOptionsResolutionPanel_Default, VideoOptionsResolutionPanel_Refresh);

	self:SetScript("OnEvent", VideoOptionsResolutionPanel_OnEvent);
end

function VideoOptionsResolutionPanel_OnEvent (self, event, ...)
	BlizzardOptionsPanel_OnEvent(self, event, ...);

	if ( event == "SET_GLUE_SCREEN" ) then
		-- don't allow systems that don't support features to enable them
		-- The cursor controls live on the Advanced tab; the gxCursor capability guard stays here so a driver
		-- without hardware cursor support never offers it.
		local anisotropic, pixelShaders, vertexShaders, trilinear, buffering, maxAnisotropy, hardwareCursor = GetVideoCaps();
		if ( not hardwareCursor ) then
			VideoOptionsAdvancedEffectsPanelHardwareCursor:SetChecked(false);
			VideoOptionsAdvancedEffectsPanelHardwareCursor:Disable();
		end
		VideoOptionsAdvancedEffectsPanelHardwareCursor.SetChecked =
			function (self, checked)
				local anisotropic, pixelShaders, vertexShaders, trilinear, buffering, maxAnisotropy, hardwareCursor = GetVideoCaps();
				if ( not hardwareCursor ) then
					checked = false;
				end
				getmetatable(self).__index.SetChecked(self, checked);
			end
		VideoOptionsAdvancedEffectsPanelHardwareCursor.Enable =
			function (self)
				local anisotropic, pixelShaders, vertexShaders, trilinear, buffering, maxAnisotropy, hardwareCursor = GetVideoCaps();
				if ( not hardwareCursor ) then
					return;
				end
				getmetatable(self).__index.Enable(self);
				local text = _G[self:GetName().."Text"];
				local fontObject = text:GetFontObject();
				_G[self:GetName().."Text"]:SetTextColor(fontObject:GetTextColor());
			end
	end
end

function VideoOptionsResolutionPanel_RefreshGammaControls ()
	if ( VideoOptionsResolutionPanelWindowed:GetChecked() ) then
		VideoOptionsResolutionPanelDesktopGamma:SetChecked();
		VideoOptionsResolutionPanelDesktopGamma:Disable();
		BlizzardOptionsPanel_Slider_Disable(VideoOptionsResolutionPanelGammaSlider);
	else
		VideoOptionsResolutionPanelDesktopGamma:Enable();
		if ( VideoOptionsResolutionPanelDesktopGamma:GetChecked() ) then
			BlizzardOptionsPanel_Slider_Disable(VideoOptionsResolutionPanelGammaSlider);
		else
			BlizzardOptionsPanel_Slider_Enable(VideoOptionsResolutionPanelGammaSlider);
		end
	end
end

function VideoOptionsResolutionPanel_SetWindowed ()
	VideoOptionsResolutionPanel_RefreshGammaControls();
	local value;
	if ( VideoOptionsResolutionPanelWindowed:GetChecked() or
		 VideoOptionsResolutionPanelDesktopGamma:GetChecked() ) then
		value = 1;
	else
		value = 0;
	end
	BlizzardOptionsPanel_SetCVarSafe("desktopGamma", value);
end

function VideoOptionsResolutionPanel_SetGamma (value)
	VideoOptionsResolutionPanel_RefreshGammaControls();
	BlizzardOptionsPanel_SetCVarSafe("desktopGamma", value);
end

function VideoOptionsResolutionPanelResolutionDropDown_OnLoad(self)
	--self.cvar = "gxResolution";

	local value = GetCurrentResolution();

	--self.defaultValue = BlizzardOptionsPanel_GetCVarDefaultSafe(self.cvar);
	self.value = value;
	self.restart = true;

	GlueDropDownMenu_SetWidth(110, self);
	GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelResolutionDropDown_Initialize);
	GlueDropDownMenu_SetSelectedID(self, value, 1);

	self.SetValue = 
		function (self, value)
			SetScreenResolution(value);
		end
	self.GetValue =
		function (self)
			return GetCurrentResolution();
		end
	self.RefreshValue = 
		function (self)
			local value = GetCurrentResolution();
			GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelResolutionDropDown_Initialize);
			GlueDropDownMenu_SetSelectedID(self, value, 1);
			self.value = value;
			self.newValue = value;
		end
end

function VideoOptionsResolutionPanelResolutionDropDown_Initialize()
	VideoOptionsResolutionPanelResolutionDropDown_LoadResolutions(GetScreenResolutions());	
end

function VideoOptionsResolutionPanelResolutionDropDown_LoadResolutions(...)
	local info = GlueDropDownMenu_CreateInfo();
	local resolution, xIndex, width, height;
	for i=1, select("#", ...) do
		resolution = (select(i, ...));
		xIndex = strfind(resolution, "x");
		width = strsub(resolution, 1, xIndex-1);
		height = strsub(resolution, xIndex+1, strlen(resolution));
		if ( width/height > 4/3 ) then
			resolution = resolution.." "..WIDESCREEN_TAG;
		end
		info.text = resolution;
		info.value = resolution;
		info.func = VideoOptionsResolutionPanelResolutionDropDown_OnClick;
		info.checked = nil;
		GlueDropDownMenu_AddButton(info);
	end
end

function VideoOptionsResolutionPanelResolutionDropDown_OnClick(self)
	local value = self:GetID();
	local dropdown = VideoOptionsResolutionPanelResolutionDropDown;
	GlueDropDownMenu_SetSelectedID(dropdown, value, 1);
	if ( dropdown.value == value ) then
		dropdown.newValue = nil;
	else
		dropdown.newValue = value;
	end
	VideoOptionsFrameApply:Enable();
end

function VideoOptionsResolutionPanelRefreshDropDown_OnLoad(self)
	self.cvar = "gxRefresh";

	local value = BlizzardOptionsPanel_GetCVarSafe(self.cvar);

	self.defaultValue = BlizzardOptionsPanel_GetCVarDefaultSafe(self.cvar);
	self.value = value;
	self.restart = true;

	GlueDropDownMenu_SetWidth(110, self);
	GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelRefreshDropDown_Initialize);
	GlueDropDownMenu_SetSelectedValue(self, value);

	self.SetValue =
		function (self, value) 
			BlizzardOptionsPanel_SetCVarSafe(self.cvar, value);
		end;
	self.GetValue =
		function (self)
			return BlizzardOptionsPanel_GetCVarSafe(self.cvar);
		end
	self.RefreshValue = 
		function (self)
			local value = BlizzardOptionsPanel_GetCVarSafe(self.cvar);
			GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelRefreshDropDown_Initialize);
			GlueDropDownMenu_SetSelectedValue(self, value);
			self.value = value;
			self.newValue = value;
		end
end

function VideoOptionsResolutionPanelRefreshDropDown_Initialize()
	VideoOptionsResolutionPanel_GetRefreshRates(GetRefreshRates());
end

function VideoOptionsResolutionPanel_GetRefreshRates(...)
	local info = GlueDropDownMenu_CreateInfo();
	if ( select("#", ...) == 1 and select(1, ...) == 0 ) then
		VideoOptionsResolutionPanelRefreshDropDownButton:Disable();
		VideoOptionsResolutionPanelRefreshDropDownLabel:SetVertexColor(GRAY_FONT_COLOR.r, GRAY_FONT_COLOR.g, GRAY_FONT_COLOR.b);
		VideoOptionsResolutionPanelRefreshDropDownText:SetVertexColor(GRAY_FONT_COLOR.r, GRAY_FONT_COLOR.g, GRAY_FONT_COLOR.b);
		return;
	end
	for i=1, select("#", ...) do
		info.text = select(i, ...)..HERTZ;
		info.func = VideoOptionsResolutionPanelRefreshDropDown_OnClick;

		if ( GlueDropDownMenu_GetSelectedValue(VideoOptionsResolutionPanelRefreshDropDown) and tonumber(GlueDropDownMenu_GetSelectedValue(VideoOptionsResolutionPanelRefreshDropDown)) == select(i, ...) ) then
			info.checked = 1;
			GlueDropDownMenu_SetText(info.text, VideoOptionsResolutionPanelRefreshDropDown);
		else
			info.checked = nil;
		end
		info.value = select(i, ...)
		GlueDropDownMenu_AddButton(info);
	end
end

function VideoOptionsResolutionPanelRefreshDropDown_OnClick(self)
	local value = self.value;
	local dropdown = VideoOptionsResolutionPanelRefreshDropDown;
	GlueDropDownMenu_SetSelectedValue(dropdown, value);
	if ( dropdown.value == value ) then
		dropdown.newValue = nil;
	else
		dropdown.newValue = value;
	end
	VideoOptionsFrameApply:Enable();
end

function VideoOptionsResolutionPanelMultiSampleDropDown_OnLoad(self)
	local value = GetCurrentMultisampleFormat();

	--self.defaultValue = 1;
	self.value = value;
	self.restart = true;

	GlueDropDownMenu_SetWidth(160, self);
	GlueDropDownMenu_SetAnchor(self, -200, 23, "TOPRIGHT", "VideoOptionsResolutionPanelMultiSampleDropDownRight", "BOTTOMRIGHT");
	GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelMultiSampleDropDown_Initialize);
	GlueDropDownMenu_SetSelectedID(self, value);

	self.SetValue = 
		function (self, value)
			SetMultisampleFormat(value);
		end;
	self.GetValue =
		function (self)
			return GetCurrentMultisampleFormat();
		end
	self.RefreshValue = 
		function (self)
			local value = GetCurrentMultisampleFormat();
			GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelMultiSampleDropDown_Initialize);
			GlueDropDownMenu_SetSelectedID(self, value);
			self.value = value;
			self.newValue = value;
		end
end

function VideoOptionsResolutionPanelMultiSampleDropDown_Initialize()
	VideoOptionsResolutionPanel_GetMultisampleFormats(GetMultisampleFormats());
end

function VideoOptionsResolutionPanel_GetMultisampleFormats(...)
	local colorBits, depthBits, multiSample;
	local info = GlueDropDownMenu_CreateInfo();
	local checked;
	local index = 1;
	for i=1, select("#", ...), 3 do
		colorBits, depthBits, multiSample = select(i, ...);
		info.text = format(MULTISAMPLING_FORMAT_STRING, colorBits, depthBits, multiSample);
		info.func = VideoOptionsResolutionPanelMultiSampleDropDown_OnClick;
		
		if ( index == GlueDropDownMenu_GetSelectedID(VideoOptionsResolutionPanelMultiSampleDropDown) ) then
			checked = 1;
			GlueDropDownMenu_SetText(info.text, VideoOptionsResolutionPanelMultiSampleDropDown);
		else
			checked = nil;
		end
		info.checked = checked;
		GlueDropDownMenu_AddButton(info);
		index = index + 1;
	end
end

function VideoOptionsResolutionPanelMultiSampleDropDown_OnClick(self)
	local value = self:GetID();
	local dropdown = VideoOptionsResolutionPanelMultiSampleDropDown;
	GlueDropDownMenu_SetSelectedID(dropdown, value);
	if ( dropdown.value == value ) then
		dropdown.newValue = nil;
	else
		dropdown.newValue = value;
	end
	VideoOptionsFrameApply:Enable();
end

function VideoOptionsResolutionPanelAntiAliasDropDown_OnLoad(self)
	-- The menu is rebuilt from nothing on every open, so its two entries are added here rather than from the
	-- driver's list the way the multisample formats are. The cvar itself carries the choice.
	self.cvar = "antiAlias";

	self.defaultValue = BlizzardOptionsPanel_GetCVarDefaultSafe(self.cvar);
	self.value = BlizzardOptionsPanel_GetCVarSafe(self.cvar);

	GlueDropDownMenu_SetWidth(160, self);
	GlueDropDownMenu_SetAnchor(self, -200, 23, "TOPRIGHT", "VideoOptionsResolutionPanelAntiAliasDropDownRight", "BOTTOMRIGHT");
	GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelAntiAliasDropDown_Initialize);
	GlueDropDownMenu_SetSelectedValue(self, self.value);

	self.SetValue =
		function (self, value)
			BlizzardOptionsPanel_SetCVarSafe(self.cvar, value);
		end;
	self.GetValue =
		function (self)
			return BlizzardOptionsPanel_GetCVarSafe(self.cvar);
		end
	self.RefreshValue =
		function (self)
			local value = BlizzardOptionsPanel_GetCVarSafe(self.cvar);
			GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelAntiAliasDropDown_Initialize);
			GlueDropDownMenu_SetSelectedValue(self, value);
			self.value = value;
			self.newValue = value;
		end
end

function VideoOptionsResolutionPanelAntiAliasDropDown_Initialize()
	VideoOptionsResolutionPanel_GetAntiAliasModes(ANTI_ALIASING_OFF, ANTI_ALIASING_FXAA);
end

function VideoOptionsResolutionPanel_GetAntiAliasModes(...)
	local info = GlueDropDownMenu_CreateInfo();
	local dropdown = VideoOptionsResolutionPanelAntiAliasDropDown;
	local selected = GlueDropDownMenu_GetSelectedValue(dropdown) or dropdown.value;

	for i = 1, select("#", ...) do
		-- The cvar takes the index, which is also the id the dropdown reads back.
		local value = tostring(i - 1);
		local checked = tonumber(selected) == i - 1;

		info.text = select(i, ...);
		info.value = value;
		info.func = VideoOptionsResolutionPanelAntiAliasDropDown_OnClick;
		info.checked = checked and 1 or nil;
		GlueDropDownMenu_AddButton(info);

		if ( checked ) then
			GlueDropDownMenu_SetText(info.text, dropdown);
		end
	end
end

function VideoOptionsResolutionPanelAntiAliasDropDown_OnClick(self)
	local value = self.value;
	local dropdown = VideoOptionsResolutionPanelAntiAliasDropDown;
	GlueDropDownMenu_SetSelectedValue(dropdown, value);
	if ( dropdown.value == value ) then
		dropdown.newValue = nil;
	else
		dropdown.newValue = value;
	end
	VideoOptionsFrameApply:Enable();
end

function VideoOptionsResolutionPanelFxaaQualityDropDown_OnLoad(self)
	self.cvar = "fxaaQuality";

	self.defaultValue = BlizzardOptionsPanel_GetCVarDefaultSafe(self.cvar);
	self.value = BlizzardOptionsPanel_GetCVarSafe(self.cvar);

	GlueDropDownMenu_SetWidth(160, self);
	GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelFxaaQualityDropDown_Initialize);
	GlueDropDownMenu_SetSelectedValue(self, self.value);

	self.SetValue =
		function (self, value)
			BlizzardOptionsPanel_SetCVarSafe(self.cvar, value);
		end;
	self.GetValue =
		function (self)
			return BlizzardOptionsPanel_GetCVarSafe(self.cvar);
		end
	self.RefreshValue =
		function (self)
			local value = BlizzardOptionsPanel_GetCVarSafe(self.cvar);
			GlueDropDownMenu_Initialize(self, VideoOptionsResolutionPanelFxaaQualityDropDown_Initialize);
			GlueDropDownMenu_SetSelectedValue(self, value);
			self.value = value;
			self.newValue = value;
		end
end

function VideoOptionsResolutionPanelFxaaQualityDropDown_Initialize()
	VideoOptionsResolutionPanel_GetFxaaQualities(FXAA_QUALITY_LOW, FXAA_QUALITY_MEDIUM, FXAA_QUALITY_HIGH, FXAA_QUALITY_ULTRA);
end

function VideoOptionsResolutionPanel_GetFxaaQualities(...)
	local info = GlueDropDownMenu_CreateInfo();
	local dropdown = VideoOptionsResolutionPanelFxaaQualityDropDown;
	local selected = GlueDropDownMenu_GetSelectedValue(dropdown) or dropdown.value;

	for i = 1, select("#", ...) do
		-- The cvar takes the index, which is also the value the dropdown reads back.
		local value = tostring(i - 1);
		local checked = tonumber(selected) == i - 1;

		info.text = select(i, ...);
		info.value = value;
		info.func = VideoOptionsResolutionPanelFxaaQualityDropDown_OnClick;
		info.checked = checked and 1 or nil;
		GlueDropDownMenu_AddButton(info);

		if ( checked ) then
			GlueDropDownMenu_SetText(info.text, dropdown);
		end
	end
end

function VideoOptionsResolutionPanelFxaaQualityDropDown_OnClick(self)
	local value = self.value;
	local dropdown = VideoOptionsResolutionPanelFxaaQualityDropDown;
	GlueDropDownMenu_SetSelectedValue(dropdown, value);
	if ( dropdown.value == value ) then
		dropdown.newValue = nil;
	else
		dropdown.newValue = value;
	end
	VideoOptionsFrameApply:Enable();
end


-- [[ Advanced Panel ]] --
-- Shadows plus the cursor pair. Shadow quality is the real shadow-mode CVar, so it changes both the method and
-- how often shadows update; shadowMode is only a read-only mirror kept for the game's own bookkeeping. The
-- resolution slider drives shadowTextureSize independently, so edge sharpness never changes the cadence.
-- [[ Shadows Panel and Advanced (cursor) Panel ]] --
-- Two tabs. Shadows holds everything about shadows: shadow quality is the real shadow-mode CVar, so it changes
-- both the method and how often shadows update, while shadowMode is only a read-only mirror kept for the game's
-- own bookkeeping; the resolution slider drives shadowTextureSize independently, so edge sharpness never
-- changes the cadence. Advanced holds the cursor pair, which is not part of any quality preset.

ADVANCED_LABEL = "Advanced";
ADVANCED_SUBTEXT = "Extra rendering options that are not part of a graphics quality preset: the cursor pair, and the reaction-coloured outline drawn on the mouseover and target units.";
CURSOR_LABEL = "Cursor";
UNIT_OUTLINE = "Unit Outline";
OPTION_TOOLTIP_UNIT_OUTLINE = "Draws a reaction-coloured outline around the unit under the cursor and your current target. A unit hidden behind terrain or a building has no outline.";
SHADOWS_LABEL = "Shadows";
SHADOWS_SUBTEXT = "Shadow quality sets the method and how often shadows update, and shadow resolution how sharp the edges are. Hardware cursor draws the cursor through the GPU; Fix Lag adds a per-frame GPU sync that can reduce cursor lag at a frame rate cost.";
OPTION_TOOLTIP_SHADOW_RESOLUTION = "Resolution of the shadow maps. Lower values give softer, blockier shadow edges and restore some frame rate. How often shadows update is not affected.";
SHADOW_RESOLUTION = SHADOW_RESOLUTION or "Shadow Resolution";
OPTION_TOOLTIP_SHADOW_QUALITY = OPTION_TOOLTIP_SHADOW_QUALITY or "Controls both the method and quality of shadows. Decreasing this may greatly improve performance.";
SHADOW_CASCADED = "Cascaded Shadows";

local ADVANCED_LEVEL_NAMES = {
	-- Steps 0-4 are the tiled, distance-gated modes. Step 5 is SHADOW_VERY_HIGH, the cascaded shadow maps
	-- that re-render every band each frame, which is why this slider genuinely changes how often shadows
	-- update rather than only their sharpness.
	ShadowQuality = { "Lowest", "Low", "Medium", "Medium High", "High", "Cascaded" },
	ShadowResolution = { "128", "256", "512", "1024", "2048" },
};

local SHADOW_SIZES = { 128, 256, 512, 1024, 2048 };

ShadowsPanelOptions = {
	ShadowQuality = { text = "SHADOW_QUALITY", minValue = 0, maxValue = 5, valueStep = 1, tooltip = OPTION_TOOLTIP_SHADOW_QUALITY },
	ShadowResolution = { text = "SHADOW_RESOLUTION", minValue = 0, maxValue = 4, valueStep = 1, tooltip = OPTION_TOOLTIP_SHADOW_RESOLUTION },
	cascadedShadows = { text = "SHADOW_CASCADED" },
};

AdvancedPanelOptions = {
	gxCursor = { text = "HARDWARE_CURSOR" },
	gxFixLag = { text = "FIX_LAG" },
	unitOutline = { text = "UNIT_OUTLINE", tooltip = OPTION_TOOLTIP_UNIT_OUTLINE },
};

local function VideoOptionsShadowsPanel_Level (value, maxValue)
	value = math.floor((tonumber(value) or 0) + 0.5);
	if ( value < 0 ) then
		return 0;
	elseif ( value > maxValue ) then
		return maxValue;
	end
	return value;
end

function VideoOptionsShadowsPanel_GetShadowQuality (self)
	-- extShadowQuality is the authority. Reading it directly avoids the mirror, which reports SHADOW_LOWEST
	-- while the shadow textures are being rebuilt and would briefly show the wrong step.
	return VideoOptionsShadowsPanel_Level(tonumber(GetCVar("extShadowQuality")) or 0, 5);
end

function VideoOptionsShadowsPanel_SetShadowQuality (value)
	SetCVar("extShadowQuality", VideoOptionsShadowsPanel_Level(value, 5));
end

function VideoOptionsShadowsPanel_GetShadowResolution (self)
	local size = tonumber(GetCVar("shadowTextureSize")) or 0;
	if ( size <= 0 ) then
		-- 0 means "the quality tier decides", so report the size actually in use rather than snapping the
		-- sentinel onto the 128 notch. Mirrors the tier rule in CShadowCache::Init().
		local mode = tonumber(GetCVar("extShadowQuality")) or 0;
		size = ( mode == 2 or mode >= 4 ) and 2048 or 1024;
	end
	-- slider steps are 0-based like every other advanced option, so the index is returned minus one;
	-- nearest notch wins, so a size set through Config.wtf still lands on a real step instead of showing blank
	local best = 1;
	for index = 1, #SHADOW_SIZES do
		if ( math.abs(SHADOW_SIZES[index] - size) < math.abs(SHADOW_SIZES[best] - size) ) then
			best = index;
		end
	end
	return best - 1;
end

function VideoOptionsShadowsPanel_SetShadowResolution (value)
	SetCVar("shadowTextureSize", SHADOW_SIZES[VideoOptionsShadowsPanel_Level(value, #SHADOW_SIZES - 1) + 1]);
end

function VideoOptionsShadowsPanel_SetupSlider (self)
	local names = ADVANCED_LEVEL_NAMES[self.label];
	local name = self:GetName();
	if ( _G[name.."Low"] ) then
		_G[name.."Low"]:SetText(names[1]);
	end
	if ( _G[name.."High"] ) then
		_G[name.."High"]:SetText(names[#names]);
	end
end

function VideoOptionsShadowsPanel_UpdateLabel (self, value)
	local names = ADVANCED_LEVEL_NAMES[self.label];
	local entry = ShadowsPanelOptions[self.label];
	local level = names[VideoOptionsShadowsPanel_Level(value, #names - 1) + 1];
	_G[self:GetName().."Text"]:SetText((_G[entry.text] or entry.text) .. ": " .. level);
end

function VideoOptionsShadowsPanelSlider_OnValueChanged (self, value)
	VideoOptionsShadowsPanel_UpdateLabel(self, value);
	VideoOptionsEffectsPanelSlider_OnValueChanged(self, value);
end

function VideoOptionsShadowsPanel_Default (self)
	RestoreVideoEffectsDefaults();
	for _, control in next, self.controls do
		control.newValue = nil;
	end
end

function VideoOptionsShadowsPanel_Refresh (self)
	BlizzardOptionsPanel_Refresh(self);
	for _, control in next, self.controls do
		if ( control.type == CONTROLTYPE_SLIDER ) then
			VideoOptionsShadowsPanel_UpdateLabel(control, control:GetValue());
		end
	end
end

function VideoOptionsShadowsPanel_OnLoad (self)
	self.name = SHADOWS_LABEL;
	self.options = ShadowsPanelOptions;
	VideoOptionsPanel_OnLoad(self, nil, nil, VideoOptionsShadowsPanel_Default, VideoOptionsShadowsPanel_Refresh);
end

function VideoOptionsAdvancedPanel_OnLoad (self)
	self.name = ADVANCED_LABEL;
	self.options = AdvancedPanelOptions;
	VideoOptionsPanel_OnLoad(self);
end

-- [[ Physics Panel ]] --
-- Grass and clutter: how much grows, how far it is drawn, and the wind that moves it. The two sliders that
-- used to sit on the Effects panel moved here so each value has one home; the rest are new. The wind's
-- direction, wavelength and stiffness rules stay in CGrassMotion as named constants.
PHYSICS_LABEL = "Physics";
PHYSICS_SUBTEXT = "Clutter and the wind that moves it. Density and view distance both grow the grass pool, which is allocated once when the setting changes.";
CLUTTER_LABEL = "Clutter";
WIND_LABEL = "Wind";
DISTANCE_LABEL = "Distance";
PARTING_LABEL = "Parting";
CLUTTER_DENSITY = "Clutter Density";
CLUTTER_RADIUS = "View Distance";
WIND_SWAY = "Sway";
WIND_FLUTTER = "Flutter";
WIND_SPEED = "Speed";
GRASS_PARTING = "Parting";
GRASS_PARTING_STRENGTH = "Strength";
GRASS_PARTING_RADIUS = "Radius";
OPTION_TOOLTIP_CLUTTER_DENSITY = "How much clutter grows per terrain cell. 196 is the ceiling, twelve times the stock maximum, and roughly triples the grass pool.";
OPTION_TOOLTIP_CLUTTER_RADIUS = "How far clutter is drawn, in yards. A map may cap this lower than the slider allows.";
OPTION_TOOLTIP_GRASS_WIND = "Sways grass and clutter in the wind. Off draws the stock, motionless grass.";
OPTION_TOOLTIP_GRASS_WIND_SWAY = "How far the tip of a blade leans at full strength, in yards.";
OPTION_TOOLTIP_GRASS_WIND_FLUTTER = "Extra per-blade shimmer on top of the sway, in yards, so a field moves as blades rather than as one sheet.";
OPTION_TOOLTIP_GRASS_WIND_SPEED = "How fast the wind travels, in radians per second.";
OPTION_TOOLTIP_OPTION_TOOLTIP_OPTION_TOOLTIP_OPTION_TOOLTIP_OPTION_TOOLTIP_GRASS_PARTING = "Lets the grass part around nearby characters, using each one's own size.";
OPTION_TOOLTIP_GRASS_PARTING_STRENGTH = "How far blades lean away from a character, in yards. The first notch is a gentle lean, not a hole.";
OPTION_TOOLTIP_GRASS_PARTING_RADIUS = "Push radius as a multiple of a character's own bounding radius. A mount adds a little more.";

PhysicsPanelOptions = {
	-- The two Effects-panel sliders, moved here. Ranges match CGrassMotion::kMinClutterDensity /
	-- kMaxClutterDensity and kMaxViewDistance, which are also the ceilings the two cvars enforce.
	groundEffectDensity = { text = "CLUTTER_DENSITY", minValue = 16, maxValue = 196, valueStep = 4, tooltip = OPTION_TOOLTIP_CLUTTER_DENSITY },
	groundEffectDist = { text = "CLUTTER_RADIUS", minValue = 20, maxValue = 196, valueStep = 4, tooltip = OPTION_TOOLTIP_CLUTTER_RADIUS },
	grassWind = { text = "WIND_LABEL", tooltip = OPTION_TOOLTIP_GRASS_WIND },
	grassWindSway = { text = "WIND_SWAY", minValue = 0, maxValue = 0.75, valueStep = 0.025, tooltip = OPTION_TOOLTIP_GRASS_WIND_SWAY },
	grassWindFlutter = { text = "WIND_FLUTTER", minValue = 0, maxValue = 0.25, valueStep = 0.01, tooltip = OPTION_TOOLTIP_GRASS_WIND_FLUTTER },
	grassWindSpeed = { text = "WIND_SPEED", minValue = 0, maxValue = 10, valueStep = 0.25, tooltip = OPTION_TOOLTIP_GRASS_WIND_SPEED },
	grassParting = { text = "GRASS_PARTING", tooltip = OPTION_TOOLTIP_GRASS_PARTING },
	grassPartingStrength = { text = "GRASS_PARTING_STRENGTH", minValue = 0, maxValue = 2, valueStep = 0.02, tooltip = OPTION_TOOLTIP_GRASS_PARTING_STRENGTH },
	grassPartingRadiusScale = { text = "GRASS_PARTING_RADIUS", minValue = 0, maxValue = 8, valueStep = 0.05, tooltip = OPTION_TOOLTIP_GRASS_PARTING_RADIUS },
};

function VideoOptionsPhysicsPanel_OnLoad (self)
	self.name = PHYSICS_LABEL;
	self.options = PhysicsPanelOptions;

	-- The panel may be a scroll frame (controls then live in a scroll child and register themselves
	-- against it via self:GetParent() in each control's OnLoad); adopt that list, or the panel would find
	-- no controls. Guarded because self is only a scroll frame when the XML that loaded is the scroll
	-- variant: on a plain frame self.controls is already the right list, and GetScrollChild does not exist.
	local child = self.GetScrollChild and self:GetScrollChild();
	if ( child and child.controls ) then
		self.controls = child.controls;
	end

	VideoOptionsPanel_OnLoad(self);
end

-- [[ Debug Panel ]] --
-- The overlay itself lives on UIParent and is built in FrameXML's copy of this file; glue only exposes the
-- checkbox, which is what writes the showPerformance CVar.
DEBUG_LABEL = "Debug";
DEBUG_SUBTEXT = "Performance overlay showing frame rate, frame time, draw calls, triangles and process CPU use. The overlay is draggable.";
PERFORMANCE_LABEL = "Performance";
PERFORMANCE_TOGGLE = "Show Performance Overlay";
OPTION_TOOLTIP_PERFORMANCE_TOGGLE = "Draws a small movable overlay with frame rate, milliseconds per frame, draw calls, triangles and CPU use.";

DebugPanelOptions = {
	showPerformance = { text = "PERFORMANCE_TOGGLE", tooltip = OPTION_TOOLTIP_PERFORMANCE_TOGGLE },
};

function VideoOptionsDebugPanel_OnLoad (self)
	self.name = DEBUG_LABEL;
	self.options = DebugPanelOptions;
	VideoOptionsPanel_OnLoad(self);
end

-- particleDensity / environmentDetail were 0.1-1.0 / 0.5-1.5 in 3.3.5, the 4.3.4 world uses percent
function VideoOptionsEffectsPanel_GetPercentCVar (self)
	local value = tonumber(GetCVar(self.cvar)) or 100;
	if ( value <= 1.5 ) then
		value = math.floor(value * 100 + 0.5);
	end
	return value;
end

-- [[ Effects Panel ]] --

EffectsPanelOptions = {
	farclip = { text = "FARCLIP", minValue = OPTIONS_FARCLIP_MIN, maxValue = OPTIONS_FARCLIP_MAX, valueStep = (OPTIONS_FARCLIP_MAX - OPTIONS_FARCLIP_MIN)/10},
	TerrainMip = { text = "TERRAIN_MIP", minValue = 0, maxValue = 1, valueStep = 1, tooltip = OPTION_TOOLTIP_TERRAIN_TEXTURE, },
	particleDensity = { text = "PARTICLE_DENSITY", minValue = 10, maxValue = 100, valueStep = 10},
	environmentDetail = { text = "ENVIRONMENT_DETAIL", minValue = 50, maxValue = 150, valueStep = 25},
	BaseMip = { text = "TEXTURE_DETAIL", minValue = 0, maxValue = 1, valueStep = 1, tooltipPoint = "BOTTOMRIGHT", tooltipOwnerPoint = "TOPLEFT", },
	textureFilteringMode = { text = "ANISOTROPIC", minValue = 0, maxValue = 5, valueStep = 1, gameRestart = 1, tooltipPoint = "BOTTOMRIGHT", tooltipOwnerPoint = "TOPLEFT", tooltipRequirement = OPTION_RESTART_REQUIREMENT, },
	weatherDensity = { text = "WEATHER_DETAIL", minValue = 0, maxValue = 3, valueStep = 1, tooltipPoint = "BOTTOMRIGHT", tooltipOwnerPoint = "TOPLEFT", },
	componentTextureLevel = { text = "PLAYER_DETAIL", minValue = 8, maxValue = 9, valueStep = 1, tooltipPoint = "BOTTOMRIGHT", tooltipOwnerPoint = "TOPLEFT", gameRestart = 1, tooltipRequirement = OPTION_RESTART_REQUIREMENT, },
	ffxGlow = { text = "FULL_SCREEN_GLOW", },
	ffxDeath = { text = "DEATH_EFFECT", },
	projectedTextures = { text = "PROJECTED_TEXTURES", },
	quality = { text = "", minValue = 1, maxValue = 6, valueStep = 1 },
}

function VideoOptionsEffectsPanel_Default (self)
	RestoreVideoEffectsDefaults();
	for _, control in next, self.controls do
		if ( control ~= VideoOptionsEffectsPanelQualitySlider ) then
			control.newValue = nil;
		end
	end
end

function VideoOptionsEffectsPanel_Refresh (self)
	BlizzardOptionsPanel_Refresh(self);
	VideoOptionsEffectsPanel_UpdateVideoQuality();
	-- HACK: force update the quality slider because the update video quality call will change the new value
	VideoOptionsEffectsPanelQualitySlider.value = VideoOptionsEffectsPanelQualitySlider.newValue;
end

function VideoOptionsEffectsPanel_OnLoad (self)
	self.name = EFFECTS_LABEL;
	self.options = EffectsPanelOptions;
	VideoOptionsPanel_OnLoad(self, nil, nil, VideoOptionsEffectsPanel_Default, VideoOptionsEffectsPanel_Refresh);

	-- this must come AFTER the parent OnLoad because the functions will be set to defaults there
	self:SetScript("OnEvent", VideoOptionsEffectsPanel_OnEvent);
end

function VideoOptionsEffectsPanel_OnEvent (self, event, ...)
	BlizzardOptionsPanel_OnEvent(self, event, ...);

	if ( event == "SET_GLUE_SCREEN" ) then
		-- fixup value steps for the farclip control, which has an adjustable min/max
		local farclipControl = VideoOptionsEffectsPanelViewDistance;
		local minValue, maxValue = farclipControl:GetMinMaxValues();
		farclipControl:SetValueStep(1);

		-- some of the values in the preset graphics quality levels aren't available on all platforms, so we
		-- need to fixup the quality levels now
		VideoOptionsEffectsPanel_FixupQualityLevels();
		VideoOptionsEffectsPanel_UpdateVideoQuality();
		
		if ( not IsPlayerResolutionAvailable() ) then
			VideoOptionsEffectsPanelPlayerTexture:Disable();
		end
	end
end

function VideoOptionsEffectsPanel_SetVideoQuality (quality)
	if ( not quality or not GraphicsQualityLevels[quality] or VideoOptionsEffectsPanel.videoQuality == quality ) then
		return;
	elseif ( quality == VIDEO_OPTIONS_CUSTOM_QUALITY ) then
		VideoOptionsEffectsPanel.videoQuality = quality;
		VideoOptionsEffectsPanel_SetVideoQualityLabels(quality);
		return;
	end

	for control, value in next, GraphicsQualityLevels[quality] do
		control = _G[control];
		if ( control.type == CONTROLTYPE_SLIDER ) then
			control:SetDisplayValue(value);
		elseif ( control.type == CONTROLTYPE_CHECKBOX ) then
			if ( value ) then
				control:SetChecked(true);
			else
				control:SetChecked(false);
			end
			BlizzardOptionsPanel_CheckButton_SetNewValue(control);
		end
	end

	VideoOptionsEffectsPanel.videoQuality = quality;
	VideoOptionsEffectsPanel_SetVideoQualityLabels(quality);
end

function VideoOptionsEffectsPanel_SetVideoQualityLabels (quality)
	quality = quality or 5;
	VideoOptionsEffectsPanelQualityLabel:SetFormattedText(VIDEO_QUALITY_S, _G["VIDEO_QUALITY_LABEL" .. quality]);
	VideoOptionsEffectsPanelQualitySubText:SetText(_G["VIDEO_QUALITY_SUBTEXT" .. quality]);
	VideoOptionsEffectsPanelQualitySlider:SetValue(quality);
end

function VideoOptionsEffectsPanel_GetVideoQuality ()
	for quality, controls in ipairs(GraphicsQualityLevels) do 
		local mismatch = false;
		for control, value in next, controls do
			control = _G[control];
			if ( control.type == CONTROLTYPE_CHECKBOX  ) then
				local checked = control:GetChecked();
				if ( ( value and not checked ) or ( not value and checked ) ) then
					mismatch = true;
					break;
				end
			elseif ( control.GetValue ) then
				if ( not (abs(control:GetValue() - value) <= VIDEO_OPTIONS_COMPARISON_EPSILON) ) then
					-- you may have been expecting ( control:GetValue() ~= value ) but here's why we can't use that:
					-- 1) floating point error: if we set a value to 0.4 and the machine's floating point error results in the value being 0.40000000596046 instead,
					--    we want those two values to be considered equal
					-- 2) NaN/IND numbers: if for whatever reason a control gives us an NaN or IND number, any comparisons with those numbers will evaluate to false,
					--    so we phrase the comparison inversely so NaN/IND comparisons result in a mismatch
					mismatch = true;
					break;
				end
			end
		end
		if ( not mismatch ) then
			return quality;
		end
	end

	return VIDEO_OPTIONS_CUSTOM_QUALITY;
end

function VideoOptionsEffectsPanel_SetCustomQuality ()
	for control in next, GraphicsQualityLevels[1] do
		control = _G[control];
		control:Enable();
	end
end

function VideoOptionsEffectsPanel_SetPresetQuality ()
	for control in next, GraphicsQualityLevels[1] do
		control = _G[control];
		control:Disable();
	end
end

function VideoOptionsEffectsPanel_UpdateVideoQuality ()
	local quality = VideoOptionsEffectsPanel_GetVideoQuality();
	if ( quality ~= VideoOptionsEffectsPanel.videoQuality ) then
		VideoOptionsEffectsPanel_SetVideoQuality(quality);
	end
end

function VideoOptionsEffectsPanelSlider_OnValueChanged (self, value)
	self.newValue = value;
	if(self:GetParent():IsVisible()) then
		VideoOptionsEffectsPanel_UpdateVideoQuality();
		VideoOptionsFrameApply:Enable();
	end
end

function VideoOptionsEffectsPanel_FixupQualityLevels ()
	-- set the lowest and highest
	for quality, controls in ipairs(GraphicsQualityLevels) do
		for index, value in next, controls do
			local control = _G[index];
			if ( control.type ~= CONTROLTYPE_CHECKBOX and control.cvar ) then
				local minValue = BlizzardOptionsPanel_GetCVarMinSafe(control.cvar);
				local maxValue = BlizzardOptionsPanel_GetCVarMaxSafe(control.cvar);
				if ( minValue and value < minValue ) then
					controls[index] = minValue;
				elseif ( maxValue and value > maxValue ) then
					controls[index] = maxValue;
				end
			end
		end
	end
end

--[[Stereo Options]]

VideoStereoPanelOptions = {
	gxStereoEnabled = { text = "ENABLE_STEREO_VIDEO" },
	gxStereoConvergence = { text = "DEPTH_CONVERGENCE", minValue = 0.2, maxValue = 50, valueStep = 0.1, tooltip = OPTION_STEREO_CONVERGENCE},
	gxStereoSeparation = { text = "EYE_SEPARATION", minValue = 0, maxValue = 100, valueStep = 1, tooltip = OPTION_STEREO_SEPARATION},
	gxCursor = { text = "STEREO_HARDWARE_CURSOR" },
}

function VideoOptionsStereoPanel_OnLoad (self)
	self.name = STEREO_VIDEO_LABEL;
	self.options = VideoStereoPanelOptions;
	if ( IsStereoVideoAvailable() ) then
		VideoOptionsPanel_OnLoad(self);
	end
	self:RegisterEvent("SET_GLUE_SCREEN");
	self:SetScript("OnEvent", VideoOptionsStereoPanel_OnEvent);
end

function VideoOptionsStereoPanel_Default (self)
	RestoreVideoStereoDefaults();
	for _, control in next, self.controls do
		if ( control.defaultValue and control.value ~= control.defaultValue ) then
			control:SetValue(control.defaultValue);
		end
		control.newValue = nil;
	end
end

function VideoOptionsStereoPanel_OnEvent(self, event, ...)
	BlizzardOptionsPanel_OnEvent(self, event, ...);
	
	if ( event == "SET_GLUE_SCREEN" ) then
		-- don't allow systems that don't support features to enable them
		local anisotropic, pixelShaders, vertexShaders, trilinear, buffering, maxAnisotropy, hardwareCursor = GetVideoCaps();
		if ( not hardwareCursor ) then
			VideoOptionsStereoPanelHardwareCursor:SetChecked(false);
			VideoOptionsStereoPanelHardwareCursor:Disable();
		end
		VideoOptionsStereoPanelHardwareCursor.SetChecked =
			function (self, checked)
				local anisotropic, pixelShaders, vertexShaders, trilinear, buffering, maxAnisotropy, hardwareCursor = GetVideoCaps();
				if ( not hardwareCursor ) then
					checked = false;
				end
				getmetatable(self).__index.SetChecked(self, checked);
			end
		VideoOptionsStereoPanelHardwareCursor.Enable =
			function (self)
				local anisotropic, pixelShaders, vertexShaders, trilinear, buffering, maxAnisotropy, hardwareCursor = GetVideoCaps();
				if ( not hardwareCursor ) then
					return;
				end
				getmetatable(self).__index.Enable(self);
				local text = _G[self:GetName().."Text"];
				local fontObject = text:GetFontObject();
				_G[self:GetName().."Text"]:SetTextColor(fontObject:GetTextColor());
			end
	end
end
