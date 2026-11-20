// Included by the native Windows platform owner, inside namespace hp2.
// All controls edit the same NativePcSettings serialized by Config.cpp.
class NativeConfigurator final {
public:
    NativeConfigurator(NativePcSettings& settings,const NativeProcessLaunch& launch,
        std::filesystem::path file)
        :settings_(settings),draft_(settings),launch_(launch),file_(std::move(file))
    {platform::MigrateNativeInputPreferences(draft_.input);}
    ~NativeConfigurator()
    {
        if(window_ && IsWindow(window_))DestroyWindow(window_);
        if(font_)DeleteObject(font_);
        if(headingFont_)DeleteObject(headingFont_);
        if(class_)UnregisterClassW(L"NFSHP2Configurator",GetModuleHandleW(nullptr));
    }
    bool Run()
    {
        // Only tabs are common controls here. BUTTON/EDIT/STATIC/LISTBOX/
        // COMBOBOX belong to USER32; requesting their v6 registration breaks
        // startup when the executable uses the default ComCtl32 v5 context.
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES};
        Require(InitCommonControlsEx(&controls)!=FALSE,"Could not initialize native settings controls");
        (void)SetProcessDPIAware();
        const auto monitor=NativeChooseMonitor(draft_.monitorDevice);
        const RECT work=monitor.work;
        const auto dc=GetDC(nullptr);Require(dc!=nullptr,"Could not query desktop DPI");
        dpi_=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);
        // Fit the complete window, including the actual Windows caption/frame.
        // A fixed pixel allowance can clip the footer on a high-DPI desktop.
        RECT frame{};
        Require(AdjustWindowRectEx(&frame,WindowStyle,FALSE,WS_EX_CONTROLPARENT)!=FALSE,
            "Could not measure native settings window frame");
        const int clientWidth=std::max(1,static_cast<int>((work.right-work.left)-(frame.right-frame.left)-24));
        const int clientHeight=std::max(1,static_cast<int>((work.bottom-work.top)-(frame.bottom-frame.top)-24));
        dpi_=std::max(64,std::min({dpi_,clientWidth*96/800,clientHeight*96/560}));
        font_=CreateFontW(-Scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        headingFont_=CreateFontW(-Scale(22),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        Require(font_ && headingFont_,"Could not create native settings fonts");
        WNDCLASSEXW type{};type.cbSize=sizeof(type);type.lpfnWndProc=Procedure;
        type.hInstance=GetModuleHandleW(nullptr);type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        type.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);type.lpszClassName=L"NFSHP2Configurator";
        NativeSetApplicationIcons(type);
        class_=RegisterClassExW(&type);Require(class_!=0,"Could not register native settings window");
        RECT extent{0,0,Scale(800),Scale(560)};
        Require(AdjustWindowRectEx(&extent,WindowStyle,FALSE,WS_EX_CONTROLPARENT)!=FALSE,
            "Could not calculate native settings window size");
        const int width=extent.right-extent.left,height=extent.bottom-extent.top;
        window_=CreateWindowExW(WS_EX_CONTROLPARENT,type.lpszClassName,L"NFSHP2 - Launcher",
            WindowStyle,work.left+(work.right-work.left-width)/2,
            work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,type.hInstance,this);
        if(!window_ && creationFailure_)std::rethrow_exception(creationFailure_);
        Require(window_!=nullptr,"Could not create native settings window");
        ShowWindow(window_,SW_SHOW);RedrawControls();SetFocus(GetDlgItem(window_,IDOK));
        MSG message{};
        while(!done_) {
            const auto result=GetMessageW(&message,nullptr,0,0);
            if(result<=0) {if(result<0)throw std::runtime_error("Settings message loop failed");break;}
            if(keyCapture_>=0 && (message.message==WM_KEYDOWN || message.message==WM_SYSKEYDOWN)) {
                if((message.lParam&(LPARAM{1}<<30))==0)CaptureKey(message);
                continue;
            }
            if(padCapture_>=0 && (message.message==WM_KEYDOWN || message.message==WM_SYSKEYDOWN) && message.wParam==VK_ESCAPE) {
                CancelCapture(L"Controller capture cancelled.");continue;
            }
            if(!IsDialogMessageW(window_,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        }
        return startGame_;
    }
private:
    enum Id:int {
        Tab=100,Monitor,RefreshMonitors,Resolution,Fullscreen,Vsync,FrameLimit,UnlockFps,ShowFps,SkipMovies,
        Filter,Antialiasing,Anisotropy,DrawDistance,MirrorDistance,OriginalGraphics,
        ModeOne,ModeTwo,DeviceOne,DeviceTwo,PlayerOneStatus,PlayerTwoStatus,RefreshDevices,Vibration,KeyboardGamepad,
        KeyboardPlayer,KeyboardDescription,KeyEscape,KeyDelete,KeyClear,ResetKeyboard,
        GamepadPlayer,GamepadDevice,ActionList,CapturePad,ClearPad,ResetPad,AxisInvert,AxisDeadzone,AxisSaturation,
        AxisDefault,TriggerDeadzone,TriggerDefault,VibrationScale,LivePreview,
        Debug,Unlock,RacerNitro,TrafficVehicles,DisableCatchUp,CockpitView,OpenProfile,
        Status,Save,Defaults,KeyBase=300
    };
    static constexpr DWORD WindowStyle=NativeFixedWindowStyle|WS_CLIPCHILDREN|WS_CLIPSIBLINGS;
    static constexpr UINT_PTR InputTimer=1;
    static constexpr std::array<const wchar_t*,4> AxisNames{{L"Right stick X",L"Throttle / brake",L"Steering",L"Left stick Y"}};
    int Scale(int value) const {return MulDiv(value,dpi_,96);}
    HWND Control(int page,const wchar_t* type,const wchar_t* label,DWORD style,int x,int y,int width,int height,int id=0)
    {
        // Page widgets remain hidden until their values and selected page are
        // ready. All widgets share this parent, so sibling clipping is needed.
        const DWORD visibility=page<0?WS_VISIBLE:0;
        auto* child=CreateWindowExW(0,type,label,WS_CHILD|WS_CLIPSIBLINGS|visibility|style,Scale(x),Scale(y),Scale(width),Scale(height),
            window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!child) {
            const auto error=GetLastError();
            throw std::runtime_error("Could not create native settings control (class="+
                std::filesystem::path(type).u8string()+", id="+std::to_string(id)+
                ", Win32="+std::to_string(error)+")");
        }
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        if(page>=0)pages_[static_cast<std::size_t>(page)].push_back(child);
        return child;
    }
    void Label(int page,const wchar_t* text,int x,int y,int width,int height=24,int id=0)
    {Control(page,L"STATIC",text,SS_LEFT|SS_NOPREFIX,x,y,width,height,id);}
    HWND Combo(int page,int id,int x,int y,int width,std::initializer_list<const wchar_t*> values)
    {
        auto* child=Control(page,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,x,y,width,230,id);
        for(const auto* value:values)SendMessageW(child,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value));
        return child;
    }
    void Button(int page,int id,const wchar_t* text,int x,int y,int width,int height=28)
    {Control(page,L"BUTTON",text,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,height,id);}
    void CheckBox(int page,int id,const wchar_t* text,int x,int y,int width)
    {Control(page,L"BUTTON",text,WS_TABSTOP|BS_AUTOCHECKBOX,x,y,width,24,id);}
    void Edit(int page,int id,int x,int y,int width)
    {
        const auto child=Control(page,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,x,y,width,25,id);
        SendMessageW(child,EM_SETLIMITTEXT,64,0);
    }
    void SetText(int id,const std::wstring& value) const {SetDlgItemTextW(window_,id,value.c_str());}
    std::wstring Text(int id) const
    {
        const auto child=GetDlgItem(window_,id);
        const auto length=std::clamp(GetWindowTextLengthW(child),0,4096);
        std::wstring value(static_cast<std::size_t>(length)+1,L'\0');
        const auto count=GetWindowTextW(child,value.data(),length+1);
        value.resize(static_cast<std::size_t>(std::max(0,count)));return value;
    }
    void CheckValue(int id,bool value) const {SendDlgItemMessageW(window_,id,BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);}
    bool Checked(int id) const {return SendDlgItemMessageW(window_,id,BM_GETCHECK,0,0)==BST_CHECKED;}
    void Select(int id,int value) const {SendDlgItemMessageW(window_,id,CB_SETCURSEL,static_cast<WPARAM>(value),0);}
    int Selected(int id) const {return static_cast<int>(SendDlgItemMessageW(window_,id,CB_GETCURSEL,0,0));}
    void AddCombo(int id,const std::wstring& value) const {SendDlgItemMessageW(window_,id,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value.c_str()));}
    void StatusText(const std::wstring& value) const {SetText(Status,value);}
    static std::wstring Number(double value)
    {
        std::wostringstream stream;stream.imbue(std::locale::classic());stream.precision(4);stream<<value;return stream.str();
    }
    static std::wstring DistanceNumber(float value)
    {
        std::wostringstream stream;stream.imbue(std::locale::classic());
        stream.precision(std::numeric_limits<float>::max_digits10);stream<<value;return stream.str();
    }
    double Numeric(int id,const char* name,double minimum,double maximum) const
    {
        auto value=Text(id);std::replace(value.begin(),value.end(),L',',L'.');
        std::wistringstream stream(value);stream.imbue(std::locale::classic());double number{};wchar_t extra{};
        if(!(stream>>number) || stream>>extra || !std::isfinite(number) || number<minimum || number>maximum)
            throw std::invalid_argument(std::string(name)+" must be between "+std::to_string(minimum)+" and "+std::to_string(maximum));
        return number;
    }
    int Percent(int id,const char* name,int range,int maximum,bool allowDefault) const
    {
        auto text=Text(id);
        std::transform(text.begin(),text.end(),text.begin(),[](wchar_t c){return c>=L'A' && c<=L'Z'?static_cast<wchar_t>(c+32):c;});
        if(allowDefault && (text==L"default" || text==L"-1"))return -1;
        return std::min(maximum*range/100,static_cast<int>(std::lround(Numeric(id,name,0,maximum)*range/100.0)));
    }
    void CreateControls()
    {
        auto* title=Control(-1,L"STATIC",L"Need for Speed Hot Pursuit 2",SS_LEFT|SS_NOPREFIX,22,15,750,30);
        SendMessageW(title,WM_SETFONT,reinterpret_cast<WPARAM>(headingFont_),TRUE);
        Label(-1,L"Display, graphics, independent player controls and advanced options.",24,47,730,22);
        auto* tabs=Control(-1,WC_TABCONTROLW,L"",WS_TABSTOP,18,78,764,406,Tab);
        const wchar_t* titles[]={L"Display",L"Graphics",L"Players",L"Keyboard",L"Gamepad",L"Advanced Settings"};
        for(int i=0;i<6;++i) {
            TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(titles[i]);
            SendMessageW(tabs,TCM_INSERTITEMW,static_cast<WPARAM>(i),reinterpret_cast<LPARAM>(&item));
        }
        CreateDisplay();CreateGraphics();CreatePlayers();CreateKeyboard();CreateGamepad();CreateAdvanced();
        Label(-1,L"",24,489,748,30,Status);
        Button(-1,Defaults,L"Defaults",22,522,139);
        Button(-1,Save,L"Save",432,522,108);
        Button(-1,IDCANCEL,L"Cancel",550,522,100);
        Control(-1,L"BUTTON",launch_.configureOnly?L"Done":L"Play",WS_TABSTOP|BS_DEFPUSHBUTTON,660,522,118,28,IDOK);
        // New child HWNDs start below older siblings. The tab was created
        // first, so explicitly put its background behind every page widget.
        // Clipping alone would otherwise preserve the tab covering the fields.
        Require(SetWindowPos(tabs,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)!=FALSE,
            "Could not arrange native settings pages");
        catalog_=std::make_unique<WindowsInputCatalog>();
        LoadControls();loading_=false;ShowPage(0);
        Require(SetTimer(window_,InputTimer,33,nullptr)!=0,"Could not start native controller discovery");
    }
    void CreateDisplay()
    {
        Label(0,L"Monitor",38,121,180);Combo(0,Monitor,238,116,420,{});Button(0,RefreshMonitors,L"Refresh",668,115,80);
        Label(0,L"Render resolution",38,167,190);
        Control(0,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWN|WS_VSCROLL,238,162,245,230,Resolution);
        Label(0,L"Render FPS limit",38,211,190);Edit(0,FrameLimit,238,207,110);Button(0,UnlockFps,L"Unlock",360,205,110);
        Label(0,L"0 = unlocked; up to 1000 FPS",484,211,255);
        CheckBox(0,Fullscreen,L"Borderless fullscreen",238,250,490);
        CheckBox(0,Vsync,L"Vertical synchronization (VSync)",238,284,490);
        CheckBox(0,ShowFps,L"Show real render FPS counter",238,318,490);
        CheckBox(0,SkipMovies,L"Skip intro movies",238,352,490);
        Label(0,L"Render FPS is independent of the game tick rate. VSync may limit presentation to the monitor refresh rate.\n"
            L"The selected render resolution and monitor apply when you start the game.",38,402,704,62);
    }
    void CreateGraphics()
    {
        Label(1,L"Texture filtering",38,123,190);
        Combo(1,Filter,238,118,245,{L"Original texture settings",L"Bilinear",L"Trilinear",L"Anisotropic"});
        Label(1,L"AF level",500,123,70);Combo(1,Anisotropy,578,118,170,{L"2x",L"4x",L"8x",L"16x"});
        Label(1,L"Anti-aliasing",38,170,190);Combo(1,Antialiasing,238,165,390,{L"Off",L"MSAA 2x",L"MSAA 4x",L"MSAA 8x"});
        Label(1,L"World draw distance",38,216,190);Edit(1,DrawDistance,238,212,110);Label(1,L"1.0 to 4.0 times original",366,216,340);
        Label(1,L"Rear mirror distance",38,262,190);Edit(1,MirrorDistance,238,258,110);Label(1,L"0.5 to 4.0 times original",366,262,340);
        Button(1,OriginalGraphics,L"Original graphics preset",38,307,245);
        Label(1,L"Higher distances draw more objects. Shadows and reflections use the original settings.",38,351,704,36);
    }
    void CreatePlayers()
    {
        Label(2,L"Player 1 input",38,121,180);
        Combo(2,ModeOne,238,116,350,{L"Automatic",L"Keyboard",L"Gamepad",L"Keyboard and gamepad",L"Disabled"});
        Combo(2,DeviceOne,38,158,710,{});Label(2,L"",38,196,710,42,PlayerOneStatus);
        Label(2,L"Player 2 input",38,252,180);
        Combo(2,ModeTwo,238,247,350,{L"Automatic",L"Keyboard",L"Gamepad",L"Keyboard and gamepad",L"Disabled"});
        Combo(2,DeviceTwo,38,289,710,{});Label(2,L"",38,327,710,42,PlayerTwoStatus);
        Button(2,KeyboardGamepad,L"Keyboard P1 + gamepad P2",38,383,285);
        CheckBox(2,Vibration,L"Enable vibration",354,387,245);Button(2,RefreshDevices,L"Refresh devices",607,383,141);
        Label(2,L"Automatic assigns free connected controllers. Saved device choices remain assigned when unplugged.\n"
            L"Keyboard profiles have independent bindings; both read the Windows keyboard input stream.",38,426,710,48);
    }
    void CreateKeyboard()
    {
        Label(3,L"Keyboard for",38,120,95);Combo(3,KeyboardPlayer,143,115,125,{L"Player 1",L"Player 2"});
        Label(3,L"Click an action, then press a key.",288,120,460,22,KeyboardDescription);
        constexpr int order[]={14,15,16,17,11,2,12,8,9,10,13,4,6,7,5,18,19,3,0,1};
        for(std::size_t i=0;i<20;++i) {
            const int x=i<10?38:417,y=153+static_cast<int>(i%10)*24;
            const auto name=NativeWideText(platform::NativeInputActionName(static_cast<std::size_t>(order[i])));
            Label(3,name.c_str(),x,y+2,205,22);
            Button(3,KeyBase+order[i],L"",x+208,y-2,122,23);
        }
        Button(3,KeyEscape,L"Bind Esc",38,408,105);Button(3,KeyDelete,L"Bind Delete",153,408,110);
        Button(3,KeyClear,L"Clear",273,408,90);Button(3,ResetKeyboard,L"Reset this keyboard",558,408,190);
        Label(3,L"Esc cancels capture; Delete clears a binding. Use the buttons above to bind these keys themselves.\n"
            L"Select an input mode on Players to enable this keyboard in the game.",38,444,710,34);
    }
    void CreateGamepad()
    {
        Label(4,L"Gamepad for",38,120,95);Combo(4,GamepadPlayer,143,115,125,{L"Player 1",L"Player 2"});
        Label(4,L"",288,119,460,25,GamepadDevice);
        Control(4,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|WS_HSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,38,151,350,245,ActionList);
        Label(4,L"Selected axis calibration",416,153,325);
        CheckBox(4,AxisInvert,L"Invert this axis",416,183,310);
        Label(4,L"Deadzone (%)",416,221,165);Edit(4,AxisDeadzone,589,216,110);
        Label(4,L"Full input at (%)",416,257,165);Edit(4,AxisSaturation,589,252,110);
        Label(4,L"",416,282,330,22,AxisDefault);
        Label(4,L"Trigger deadzone (%)",416,314,170);Edit(4,TriggerDeadzone,589,309,110);
        Label(4,L"",416,337,330,20,TriggerDefault);
        Label(4,L"Vibration strength (%)",416,367,170);Edit(4,VibrationScale,589,362,110);
        Button(4,CapturePad,L"Capture",38,407,105);Button(4,ClearPad,L"Clear",153,407,90);
        Button(4,ResetPad,L"Reset profile",253,407,135);
        Label(4,L"",416,401,330,73,LivePreview);
        Label(4,L"Capture buttons, triggers, sticks or hats.\nEnter Default for an inherited deadzone.",38,443,350,34);
    }
    void CreateAdvanced()
    {
        CheckBox(5,Debug,L"Enable debug menu and debug cameras",38,122,710);
        CheckBox(5,Unlock,L"Unlock obtainable cars, tracks and events",38,157,710);
        CheckBox(5,RacerNitro,L"Enable nitro for racers",38,192,710);
        CheckBox(5,TrafficVehicles,L"Allow driving traffic vehicles",38,227,710);
        CheckBox(5,DisableCatchUp,L"Disable Catch Up in all race modes",38,262,710);
        CheckBox(5,CockpitView,L"Enable cockpit view for supported single-player cars",38,297,710);
        Label(5,L"Nitro is configurable for each player on Keyboard and Gamepad.\n"
            L"Unlocking content preserves earned points, wins and career progress.",38,335,710,46);
        Label(5,L"Saves and settings",38,390,565);
        Control(5,L"STATIC",launch_.profileDirectory.c_str(),SS_PATHELLIPSIS|SS_NOPREFIX,38,422,560,24);
        Button(5,OpenProfile,L"Open folder",616,417,132);
        Label(5,L"Start directly with saved settings using --no-configurator.",38,454,710,22);
    }

    struct Loading final {
        bool& value;bool previous;
        explicit Loading(bool& flag):value(flag),previous(flag){value=true;}
        ~Loading(){value=previous;}
    };
    void RefreshMonitorsList()
    {
        Loading guard(loading_);
        monitors_=NativeWindowsMonitors();monitorKeys_.clear();monitorKeys_.push_back("");
        SendDlgItemMessageW(window_,Monitor,CB_RESETCONTENT,0,0);AddCombo(Monitor,L"Primary monitor (automatic)");
        int chosen=draft_.monitorDevice.empty()?0:-1;
        for(const auto& monitor:monitors_) {
            AddCombo(Monitor,monitor.name);monitorKeys_.push_back(monitor.id);
            if(monitor.id==draft_.monitorDevice)chosen=static_cast<int>(monitorKeys_.size())-1;
        }
        if(chosen<0) {
            AddCombo(Monitor,L"Unavailable saved monitor - "+NativeWideText(draft_.monitorDevice));
            monitorKeys_.push_back(draft_.monitorDevice);chosen=static_cast<int>(monitorKeys_.size())-1;
        }
        Select(Monitor,chosen);RefreshResolutionModes();
    }
    void RefreshResolutionModes()
    {
        const auto current=Text(Resolution);
        const auto selected=Selected(Monitor);
        const auto key=selected>=0 && static_cast<std::size_t>(selected)<monitorKeys_.size()?monitorKeys_[static_cast<std::size_t>(selected)]:std::string{};
        const auto monitor=NativeChooseMonitor(key);const auto name=NativeWideText(monitor.id);
        std::vector<std::pair<int,int>> modes;
        DEVMODEW mode{};mode.dmSize=sizeof(mode);
        for(DWORD index=0;EnumDisplaySettingsW(name.empty()?nullptr:name.c_str(),index,&mode);++index)
            if(mode.dmPelsWidth>=320 && mode.dmPelsHeight>=240)
                modes.emplace_back(static_cast<int>(mode.dmPelsWidth),static_cast<int>(mode.dmPelsHeight));
        modes.emplace_back(draft_.width,draft_.height);
        std::sort(modes.begin(),modes.end());modes.erase(std::unique(modes.begin(),modes.end()),modes.end());
        Loading guard(loading_);SendDlgItemMessageW(window_,Resolution,CB_RESETCONTENT,0,0);
        for(const auto& size:modes)AddCombo(Resolution,std::to_wstring(size.first)+L" x "+std::to_wstring(size.second));
        SetText(Resolution,current.empty()?std::to_wstring(draft_.width)+L" x "+std::to_wstring(draft_.height):current);
    }
    void LoadGraphics()
    {
        Loading guard(loading_);
        Select(Filter,static_cast<int>(draft_.textureFilter));
        const auto aa=static_cast<int>(draft_.antialiasing);Select(Antialiasing,aa<2?0:aa-1);
        Select(Anisotropy,draft_.anisotropy==2?0:draft_.anisotropy==4?1:draft_.anisotropy==8?2:3);
        EnableWindow(GetDlgItem(window_,Anisotropy),draft_.textureFilter==NativeTextureFilter::Anisotropic);
        SetText(DrawDistance,DistanceNumber(draft_.render.drawDistanceScale));SetText(MirrorDistance,DistanceNumber(draft_.render.mirrorDistanceScale));
    }
    void LoadControls()
    {
        Loading guard(loading_);
        CancelCapture(L"");
        SetText(Resolution,std::to_wstring(draft_.width)+L" x "+std::to_wstring(draft_.height));
        RefreshMonitorsList();SetText(FrameLimit,std::to_wstring(draft_.frameLimit));
        CheckValue(Fullscreen,draft_.fullscreen);CheckValue(Vsync,draft_.vsync);CheckValue(ShowFps,draft_.showFps);
        CheckValue(SkipMovies,draft_.skipMovies);CheckValue(Vibration,draft_.input.vibration);
        LoadGraphics();
        Select(ModeOne,static_cast<int>(draft_.input.deviceMode[0]));Select(ModeTwo,static_cast<int>(draft_.input.deviceMode[1]));
        Select(KeyboardPlayer,keyboardPlayer_);Select(GamepadPlayer,gamepadPlayer_);
        CheckValue(Debug,draft_.debugTools);CheckValue(Unlock,draft_.unlockAll);
        CheckValue(RacerNitro,draft_.gameplay.racerNitro);CheckValue(TrafficVehicles,draft_.gameplay.trafficVehicles);
        CheckValue(DisableCatchUp,draft_.gameplay.disableCatchUp);CheckValue(CockpitView,draft_.gameplay.cockpitView);
        RefreshDevicesList(true);RefreshKeys();RefreshGamepadProfile();
    }
    std::wstring KeyName(std::uint16_t key) const
    {
        if(!key)return L"Unbound";
        UINT scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC);
        if(key==VK_LEFT || key==VK_RIGHT || key==VK_UP || key==VK_DOWN || key==VK_DELETE ||
            key==VK_INSERT || key==VK_HOME || key==VK_END || key==VK_PRIOR || key==VK_NEXT || key==VK_RCONTROL || key==VK_RMENU)
            scan|=0x100;
        wchar_t name[64]{};
        if(GetKeyNameTextW(static_cast<LONG>(scan<<16),name,64)>0)return name;
        return L"VK "+std::to_wstring(key);
    }
    std::array<std::uint16_t,20>& Keyboard()
    {return keyboardPlayer_==0?draft_.input.keys:draft_.input.player2Keys;}
    void RefreshKeys()
    {
        const auto& keys=Keyboard();
        for(std::size_t i=0;i<keys.size();++i)SetText(KeyBase+static_cast<int>(i),
            static_cast<int>(i)==keyCapture_?L"Press a key...":KeyName(keys[i]));
        SetText(KeyboardDescription,L"Selected: "+NativeWideText(platform::NativeInputActionName(static_cast<std::size_t>(selectedKey_))));
    }
    static bool DevicesChanged(const std::vector<platform::NativeInputDeviceInfo>& before,
        const std::vector<platform::NativeInputDeviceInfo>& after)
    {
        if(before.size()!=after.size())return true;
        for(std::size_t i=0;i<before.size();++i)
            if(before[i].id!=after[i].id || before[i].index!=after[i].index ||
                before[i].name!=after[i].name || before[i].instanceId!=after[i].instanceId || before[i].pathKey!=after[i].pathKey)return true;
        return false;
    }
    std::wstring DeviceName(const platform::NativeInputDeviceInfo& device) const
    {
        auto name=NativeWideText(device.name);
        const auto count=std::count_if(devices_.begin(),devices_.end(),[&](const auto& entry){return entry.name==device.name;});
        if(count>1)name+=L" ("+std::to_wstring(device.index+1)+L")";
        if(!device.standardized)name+=L" [joystick]";
        return name;
    }
    int SelectedDevice(int player) const
    {return platform::ResolveNativePlayerDevice(draft_.input,devices_,player);}
    platform::NativePadSample SampleFor(int player)
    {
        const auto chosen=SelectedDevice(player);
        return chosen<0?platform::NativePadSample{}:catalog_->Read(devices_[static_cast<std::size_t>(chosen)].id,-1);
    }
    void RefreshDeviceCombo(int player)
    {
        const auto port=static_cast<std::size_t>(player);const int id=player==0?DeviceOne:DeviceTwo;
        if(SendDlgItemMessageW(window_,id,CB_GETDROPPEDSTATE,0,0)!=0)return;
        Loading guard(loading_);auto& choices=deviceKeys_[port];choices.clear();choices.push_back("");
        SendDlgItemMessageW(window_,id,CB_RESETCONTENT,0,0);
        const auto resolved=SelectedDevice(player);
        AddCombo(id,L"Automatic"+(draft_.input.deviceKey[port].empty() && resolved>=0?
            L" - "+DeviceName(devices_[static_cast<std::size_t>(resolved)]):std::wstring{}));
        int selected=draft_.input.deviceKey[port].empty()?0:-1;
        for(const auto& device:devices_) {
            const auto& saved=draft_.input.deviceKey[port];
            const bool matches=device.id==saved || (!device.pathKey.empty() && device.pathKey==saved);
            AddCombo(id,DeviceName(device));choices.push_back(matches?saved:device.id);
            if(matches)selected=static_cast<int>(choices.size())-1;
        }
        if(selected<0) {
            AddCombo(id,L"Unavailable saved device - reconnect or choose another");
            choices.push_back(draft_.input.deviceKey[port]);selected=static_cast<int>(choices.size())-1;
        }
        Select(id,selected);
        const auto mode=draft_.input.deviceMode[port];
        EnableWindow(GetDlgItem(window_,id),mode!=platform::NativeDeviceMode::Keyboard && mode!=platform::NativeDeviceMode::Disabled);
    }
    void RefreshPlayerStatus()
    {
        for(int player=0;player<2;++player) {
            const auto port=static_cast<std::size_t>(player);
            const auto mode=draft_.input.deviceMode[port];std::wstring text;
            const auto chosen=SelectedDevice(player);
            if(mode==platform::NativeDeviceMode::Disabled)text=L"Player disabled.";
            else if(mode==platform::NativeDeviceMode::Keyboard)text=L"Keyboard profile "+std::to_wstring(player+1)+L" is active.";
            else if(!catalog_->Available())text=NativeWideText(catalog_->Error());
            else if(chosen>=0) {
                const auto& device=devices_[static_cast<std::size_t>(chosen)];
                text=DeviceName(device)+(device.standardized?L" - recognized SDL2 controller.":L" - assign controls on the Gamepad tab.");
                if(platform::NativeKeyboardEnabled(draft_.input,player))text+=L" Keyboard profile is also active.";
                if(device.id.find("/connection/")!=std::string::npos)text+=L" This device must be selected again after reconnecting.";
            } else if(!draft_.input.deviceKey[port].empty())text=L"Saved device unavailable. It will reconnect to this player when detected.";
            else text=L"Waiting for a free controller."+(platform::NativeKeyboardEnabled(draft_.input,player)?std::wstring(L" Keyboard profile is active."):std::wstring{});
            SetText(player==0?PlayerOneStatus:PlayerTwoStatus,text);
        }
        const auto chosen=SelectedDevice(gamepadPlayer_);
        SetText(GamepadDevice,chosen>=0?DeviceName(devices_[static_cast<std::size_t>(chosen)]):L"Select a connected gamepad on Players.");
    }
    void RefreshDevicesList(bool force)
    {
        const auto current=catalog_->Enumerate();const bool changed=DevicesChanged(devices_,current);
        devices_=current;
        if(force || changed)for(int player=0;player<2;++player)RefreshDeviceCombo(player);
        RefreshPlayerStatus();lastDiscovery_=GetTickCount64();
        if(changed && page_==4)RefreshActionList();
    }
    bool StandardizedPlayer() const
    {
        const auto selected=SelectedDevice(gamepadPlayer_);
        return selected<0 || devices_[static_cast<std::size_t>(selected)].standardized;
    }
    platform::NativeGamepadProfile& Profile()
    {return draft_.input.profiles[static_cast<std::size_t>(gamepadPlayer_)];}
    void RefreshActionList()
    {
        Loading guard(loading_);
        const auto& profile=Profile();const bool standardized=StandardizedPlayer();
        SendDlgItemMessageW(window_,ActionList,LB_RESETCONTENT,0,0);
        for(std::size_t i=0;i<20;++i) {
            std::wstring text;
            if(i<16)text=NativeWideText(platform::NativeInputActionName(i))+L" - "+NativeWideText(platform::NativeBindingName(profile.buttons[i],standardized));
            else {
                const auto axis=i-16;
                const platform::NativeInputBinding binding{profile.axes[axis]<0?platform::NativeBindingKind::None:
                    profile.invert[axis]?platform::NativeBindingKind::AxisNegative:platform::NativeBindingKind::AxisPositive,profile.axes[axis],0};
                text=std::wstring(AxisNames[axis])+L" - "+NativeWideText(platform::NativeBindingName(binding,standardized));
            }
            SendDlgItemMessageW(window_,ActionList,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
        }
        SendDlgItemMessageW(window_,ActionList,LB_SETCURSEL,static_cast<WPARAM>(selectedAction_),0);
        SendDlgItemMessageW(window_,ActionList,LB_SETHORIZONTALEXTENT,static_cast<WPARAM>(Scale(700)),0);
    }
    void RefreshAxisControls()
    {
        Loading guard(loading_);const auto& profile=Profile();const bool axis=selectedAction_>=16;
        for(const int id:{AxisInvert,AxisDeadzone,AxisSaturation})EnableWindow(GetDlgItem(window_,id),axis);
        if(axis) {
            const auto index=static_cast<std::size_t>(selectedAction_-16);
            CheckValue(AxisInvert,profile.invert[index]);
            SetText(AxisDeadzone,profile.deadzone[index]<0?L"Default":Number(profile.deadzone[index]*100.0/32767));
            SetText(AxisSaturation,Number(profile.saturation[index]*100.0/32767));
            const auto inherited=index<2?draft_.input.rightDeadzone:draft_.input.leftDeadzone;
            SetText(AxisDefault,L"Default deadzone: "+Number(inherited*100.0/32767)+L"%");
        } else {
            CheckValue(AxisInvert,false);SetText(AxisDeadzone,L"");SetText(AxisSaturation,L"");
            SetText(AxisDefault,L"Select a stick axis to edit its calibration.");
        }
        axisDeadzoneDirty_=false;axisSaturationDirty_=false;axisInvertDirty_=false;
    }
    void RefreshGamepadProfile()
    {
        Loading guard(loading_);const auto& profile=Profile();
        RefreshActionList();RefreshAxisControls();RefreshPlayerStatus();
        SetText(TriggerDeadzone,profile.triggerThreshold<0?L"Default":Number(profile.triggerThreshold*100.0/255));
        SetText(TriggerDefault,L"Default trigger deadzone: "+Number(draft_.input.triggerThreshold*100.0/255)+L"%");
        SetText(VibrationScale,std::to_wstring(profile.vibrationScale));
        triggerDirty_=false;vibrationDirty_=false;
        RefreshLivePreview(SampleFor(gamepadPlayer_));
    }
    void StoreCalibration()
    {
        auto copy=draft_.input;auto& profile=copy.profiles[static_cast<std::size_t>(gamepadPlayer_)];
        if(selectedAction_>=16) {
            const auto axis=static_cast<std::size_t>(selectedAction_-16);
            if(axisInvertDirty_)profile.invert[axis]=Checked(AxisInvert);
            if(axisDeadzoneDirty_)profile.deadzone[axis]=Percent(AxisDeadzone,"Axis deadzone",32767,90,true);
            if(axisSaturationDirty_)profile.saturation[axis]=Percent(AxisSaturation,"Axis saturation",32767,100,false);
        }
        if(triggerDirty_)profile.triggerThreshold=Percent(TriggerDeadzone,"Trigger deadzone",255,90,true);
        if(vibrationDirty_)profile.vibrationScale=static_cast<int>(std::lround(Numeric(VibrationScale,"Vibration strength",0,100)));
        // A conflicting device assignment is reported when saving Players;
        // calibration can still be repaired while both combos are being edited.
        copy.deviceMode={{platform::NativeDeviceMode::Keyboard,platform::NativeDeviceMode::Keyboard}};
        platform::ValidateNativeInputSettings(copy);
        Profile()=profile;
        axisInvertDirty_=false;axisDeadzoneDirty_=false;axisSaturationDirty_=false;triggerDirty_=false;vibrationDirty_=false;
    }

    NativePcSettings ReadControls()
    {
        StoreCalibration();auto result=draft_;
        std::wistringstream stream(Text(Resolution));wchar_t separator{},extra{};
        if(!(stream>>result.width>>separator>>result.height) ||
            (separator!=L'x' && separator!=L'X' && separator!=L'×') || stream>>extra)
            throw std::invalid_argument("Resolution must be written as 1280 x 720");
        const auto monitor=Selected(Monitor);
        if(monitor<0 || static_cast<std::size_t>(monitor)>=monitorKeys_.size())throw std::invalid_argument("Select a monitor");
        result.monitorDevice=monitorKeys_[static_cast<std::size_t>(monitor)];
        const auto limit=Numeric(FrameLimit,"FPS limit",0,1000);
        if(std::floor(limit)!=limit)throw std::invalid_argument("FPS limit must be a whole number; 0 unlocks it");
        result.frameLimit=static_cast<int>(limit);
        result.fullscreen=Checked(Fullscreen);result.vsync=Checked(Vsync);result.showFps=Checked(ShowFps);result.skipMovies=Checked(SkipMovies);
        const auto filter=Selected(Filter),aa=Selected(Antialiasing),af=Selected(Anisotropy);
        if(filter<0 || filter>3 || aa<0 || aa>3 || af<0 || af>3)
            throw std::invalid_argument("Select supported graphics options");
        result.textureFilter=static_cast<NativeTextureFilter>(filter);result.antialiasing=static_cast<NativeAntialiasing>(aa==0?0:aa+1);
        result.anisotropy=2<<af;
        result.render.drawDistanceScale=static_cast<float>(Numeric(DrawDistance,"World draw distance",1,4));
        result.render.mirrorDistanceScale=static_cast<float>(Numeric(MirrorDistance,"Rear mirror distance",0.5,4));
        result.input.vibration=Checked(Vibration);
        result.debugTools=Checked(Debug);result.unlockAll=Checked(Unlock);
        result.gameplay.racerNitro=Checked(RacerNitro);result.gameplay.trafficVehicles=Checked(TrafficVehicles);
        result.gameplay.disableCatchUp=Checked(DisableCatchUp);result.gameplay.cockpitView=Checked(CockpitView);
        validateNativePcSettings(result);return result;
    }
    void CancelCapture(const std::wstring& reason)
    {
        keyCapture_=-1;padCapture_=-1;captureArmed_=false;
        if(window_) {RefreshKeys();SetText(CapturePad,L"Capture");if(!reason.empty())StatusText(reason);}
    }
    void RedrawControls()
    {
        UINT flags=RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN;
        if(IsWindowVisible(window_))flags|=RDW_UPDATENOW;
        Require(RedrawWindow(window_,nullptr,nullptr,flags)!=FALSE,"Could not redraw native settings controls");
    }
    void ShowPage(int page)
    {
        if(page<0 || page>=static_cast<int>(pages_.size()))return;
        if(page_==4 && page!=page_) {
            try {StoreCalibration();} catch(...) {
                SendDlgItemMessageW(window_,Tab,TCM_SETCURSEL,static_cast<WPARAM>(page_),0);throw;
            }
        }
        // Do not leave keyboard focus in a page that is about to be hidden.
        const auto focus=GetFocus();
        if(page!=page_)for(const auto child:pages_[static_cast<std::size_t>(page_)])
            if(focus==child || IsChild(child,focus)) {SetFocus(GetDlgItem(window_,Tab));break;}
        CancelCapture(L"");page_=page;
        // Fill mappings/calibration/live status before exposing the page.
        if(page==2)RefreshDevicesList(true);
        if(page==4)RefreshGamepadProfile();
        for(std::size_t i=0;i<pages_.size();++i)if(static_cast<int>(i)!=page)
            for(const auto child:pages_[i])ShowWindow(child,SW_HIDE);
        for(const auto child:pages_[static_cast<std::size_t>(page)])ShowWindow(child,SW_SHOW);
        StatusText(L"");
        RedrawControls();
    }
    void CaptureKey(const MSG& message)
    {
        auto key=static_cast<UINT>(message.wParam);
        if(key==VK_SHIFT || key==VK_CONTROL || key==VK_MENU) {
            const auto scan=static_cast<UINT>((message.lParam>>16)&0xff)|((message.lParam&(LPARAM{1}<<24))?0xe000u:0u);
            key=MapVirtualKeyW(scan,MAPVK_VSC_TO_VK_EX);
        }
        if(key!=VK_ESCAPE && key<=254)Keyboard()[static_cast<std::size_t>(keyCapture_)]=key==VK_DELETE?0:static_cast<std::uint16_t>(key);
        CancelCapture(key==VK_ESCAPE?L"Keyboard capture cancelled.":L"Keyboard binding updated. Press Save or Play to keep it.");
    }
    void StartPadCapture()
    {
        StoreCalibration();RefreshDevicesList(true);
        const auto sample=SampleFor(gamepadPlayer_);
        if(!sample.connected)throw std::invalid_argument("Select a connected controller for this player on the Players tab first");
        CancelCapture(L"");padCapture_=selectedAction_;captureBaseline_=sample;
        captureStarted_=GetTickCount64();captureRested_=captureStarted_;captureArmed_=false;
        SetText(CapturePad,L"Cancel");StatusText(L"Release the controls. Capture will arm when they are still; Esc cancels.");
    }
    static bool Released(const platform::NativePadSample& sample)
    {
        for(int i=0;i<std::min(sample.buttonCount,64);++i)if(sample.rawButtons[static_cast<std::size_t>(i)])return false;
        for(int i=0;i<std::min(sample.hatCount,4);++i)if(sample.hats[static_cast<std::size_t>(i)])return false;
        if(sample.standardized) {
            for(std::size_t i=0;i<4;++i)if(std::abs(static_cast<int>(sample.rawAxes[i]))>14000)return false;
            if(sample.rawAxes[4]>5000 || sample.rawAxes[5]>5000)return false;
        }
        return true;
    }
    static bool Still(const platform::NativePadSample& sample,const platform::NativePadSample& previous)
    {
        for(int i=0;i<std::min(sample.axisCount,16);++i) {
            const auto index=static_cast<std::size_t>(i);
            if(std::abs(static_cast<int>(sample.rawAxes[index])-previous.rawAxes[index])>1500)return false;
        }
        return true;
    }
    void PollPadCapture(const platform::NativePadSample& sample,ULONGLONG now)
    {
        if(padCapture_<0)return;
        if(!sample.connected || sample.instanceId!=captureBaseline_.instanceId) {
            CancelCapture(L"The selected controller disconnected or changed. Reconnect it, then capture again.");return;
        }
        if(GetForegroundWindow()!=window_) {CancelCapture(L"Controller capture cancelled when the launcher lost focus.");return;}
        if(now-captureStarted_>15000) {CancelCapture(L"Capture timed out. Click Capture and try again.");return;}
        if(!captureArmed_) {
            if(!Released(sample) || !Still(sample,captureBaseline_))captureRested_=now;
            captureBaseline_=sample;
            if(now-captureRested_>=250) {
                captureArmed_=true;
                if(padCapture_>=16)StatusText((padCapture_==16 || padCapture_==18)?
                    L"Move the desired axis RIGHT. Esc cancels.":L"Move the desired axis DOWN. Esc cancels.");
                else StatusText(L"Press the desired button, trigger or hat, or move a stick. Esc cancels.");
            }
            return;
        }
        platform::NativeInputBinding binding;
        if(!platform::DetectNativeBinding(sample,captureBaseline_,binding,padCapture_>=16))return;
        auto& profile=Profile();
        if(padCapture_<16)profile.buttons[static_cast<std::size_t>(padCapture_)]=binding;
        else {
            const auto axis=static_cast<std::size_t>(padCapture_-16);profile.axes[axis]=binding.index;
            profile.invert[axis]=binding.kind==platform::NativeBindingKind::AxisNegative || binding.kind==platform::NativeBindingKind::AxisFullNegative;
        }
        const auto physical=NativeWideText(platform::NativeBindingName(binding,sample.standardized));
        CancelCapture(L"Captured "+physical+L". Press Save or Play to keep this binding.");RefreshGamepadProfile();
    }
    void RefreshLivePreview(const platform::NativePadSample& sample)
    {
        if(!sample.connected) {SetText(LivePreview,L"No connected controller for this player.\nChoose a device on Players.");return;}
        std::wstring text;
        if(selectedAction_>=16) {
            const auto source=Profile().axes[static_cast<std::size_t>(selectedAction_-16)];
            text=source>=0 && source<sample.axisCount?L"Selected axis: "+std::to_wstring(source+1)+L" = "+std::to_wstring(sample.rawAxes[static_cast<std::size_t>(source)]):L"Selected axis: unbound";
        } else {
            const auto& binding=Profile().buttons[static_cast<std::size_t>(selectedAction_)];
            text=NativeWideText(platform::NativeBindingName(binding,sample.standardized));
            if(binding.kind==platform::NativeBindingKind::Button && binding.index>=0 && binding.index<sample.buttonCount)
                text+=sample.rawButtons[static_cast<std::size_t>(binding.index)]?L": pressed":L": released";
            else if(binding.kind==platform::NativeBindingKind::Hat && binding.index>=0 && binding.index<sample.hatCount)
                text+=(sample.hats[static_cast<std::size_t>(binding.index)]&binding.direction)!=0?L": pressed":L": released";
            else if(binding.kind!=platform::NativeBindingKind::None && binding.index>=0 && binding.index<sample.axisCount)
                text+=L": "+std::to_wstring(sample.rawAxes[static_cast<std::size_t>(binding.index)]);
        }
        if(sample.standardized)text+=L"\nLX "+std::to_wstring(sample.rawAxes[0])+L"  LY "+std::to_wstring(sample.rawAxes[1])+
            L"  LT "+std::to_wstring(sample.rawAxes[4])+L"\nRX "+std::to_wstring(sample.rawAxes[2])+L"  RY "+std::to_wstring(sample.rawAxes[3])+L"  RT "+std::to_wstring(sample.rawAxes[5]);
        else {
            text+=L"\nPressed:";bool any=false;
            for(int i=0;i<std::min(sample.buttonCount,64);++i)if(sample.rawButtons[static_cast<std::size_t>(i)]) {
                text+=L" "+std::to_wstring(i+1);any=true;
            }
            if(!any)text+=L" none";
            text+=L"\nHats:";for(int i=0;i<std::min(sample.hatCount,4);++i)text+=L" "+std::to_wstring(sample.hats[static_cast<std::size_t>(i)]);
        }
        SetText(LivePreview,text);
    }
    void Timer()
    {
        if(loading_ || !catalog_)return;
        const auto now=GetTickCount64();
        if(now-lastDiscovery_>=500)RefreshDevicesList(false);
        if(page_==4 || padCapture_>=0) {
            const auto sample=SampleFor(gamepadPlayer_);RefreshLivePreview(sample);PollPadCapture(sample,now);
        }
    }
    void ChangedMode(int player)
    {
        const auto value=Selected(player==0?ModeOne:ModeTwo);if(value<0 || value>4)return;
        CancelCapture(L"");const auto port=static_cast<std::size_t>(player);
        draft_.input.deviceMode[port]=static_cast<platform::NativeDeviceMode>(value);
        if((value==0 || value==2 || value==3) && draft_.input.controller[port]<0)draft_.input.controller[port]=player;
        RefreshDevicesList(true);RefreshPlayerStatus();
    }
    void ChangedDevice(int player)
    {
        const auto port=static_cast<std::size_t>(player);const int chosen=Selected(player==0?DeviceOne:DeviceTwo);
        if(chosen<0 || static_cast<std::size_t>(chosen)>=deviceKeys_[port].size())return;
        CancelCapture(L"");draft_.input.deviceKey[port]=deviceKeys_[port][static_cast<std::size_t>(chosen)];
        if(chosen==0 && draft_.input.controller[port]<0)draft_.input.controller[port]=player;
        for(const auto& device:devices_)if(device.id==draft_.input.deviceKey[port] ||
            (!device.pathKey.empty() && device.pathKey==draft_.input.deviceKey[port]))draft_.input.controller[port]=device.index;
        RefreshDevicesList(true);RefreshGamepadProfile();
    }
    void Command(int id,int notification)
    {
        if(loading_)return;
        if(notification==EN_CHANGE) {
            if(id==AxisDeadzone)axisDeadzoneDirty_=true;
            if(id==AxisSaturation)axisSaturationDirty_=true;
            if(id==TriggerDeadzone)triggerDirty_=true;
            if(id==VibrationScale)vibrationDirty_=true;
            return;
        }
        if(id==AxisInvert && notification==BN_CLICKED) {axisInvertDirty_=true;return;}
        if(id==Filter && notification==CBN_SELCHANGE) {
            EnableWindow(GetDlgItem(window_,Anisotropy),Selected(Filter)==static_cast<int>(NativeTextureFilter::Anisotropic));return;
        }
        if(id==Monitor && notification==CBN_SELCHANGE) {
            const auto chosen=Selected(Monitor);
            if(chosen>=0 && static_cast<std::size_t>(chosen)<monitorKeys_.size())draft_.monitorDevice=monitorKeys_[static_cast<std::size_t>(chosen)];
            RefreshResolutionModes();return;
        }
        if((id==ModeOne || id==ModeTwo) && notification==CBN_SELCHANGE) {ChangedMode(id==ModeOne?0:1);return;}
        if((id==DeviceOne || id==DeviceTwo) && notification==CBN_SELCHANGE) {ChangedDevice(id==DeviceOne?0:1);return;}
        if((id==DeviceOne || id==DeviceTwo) && notification==CBN_CLOSEUP) {RefreshDevicesList(true);return;}
        if(id==KeyboardPlayer && notification==CBN_SELCHANGE) {
            CancelCapture(L"");keyboardPlayer_=std::clamp(Selected(KeyboardPlayer),0,1);RefreshKeys();return;
        }
        if(id==GamepadPlayer && notification==CBN_SELCHANGE) {
            try {StoreCalibration();} catch(...) {Select(GamepadPlayer,gamepadPlayer_);throw;}
            CancelCapture(L"");gamepadPlayer_=std::clamp(Selected(GamepadPlayer),0,1);RefreshGamepadProfile();return;
        }
        if(id==ActionList && notification==LBN_SELCHANGE) {
            try {StoreCalibration();} catch(...) {
                SendDlgItemMessageW(window_,ActionList,LB_SETCURSEL,static_cast<WPARAM>(selectedAction_),0);throw;
            }
            CancelCapture(L"");selectedAction_=std::clamp(static_cast<int>(SendDlgItemMessageW(window_,ActionList,LB_GETCURSEL,0,0)),0,19);
            RefreshActionList();RefreshAxisControls();return;
        }
        if(notification!=BN_CLICKED)return;
        if(id>=KeyBase && id<KeyBase+20) {
            CancelCapture(L"");selectedKey_=id-KeyBase;keyCapture_=selectedKey_;RefreshKeys();
            StatusText(L"Press a key. Esc cancels; Delete clears. Bind Esc and Bind Delete are available below.");return;
        }
        if(id==CapturePad) {if(padCapture_>=0)CancelCapture(L"Controller capture cancelled.");else StartPadCapture();return;}
        if(id==ClearPad) {
            StoreCalibration();CancelCapture(L"");
            if(selectedAction_<16)Profile().buttons[static_cast<std::size_t>(selectedAction_)]={};
            else Profile().axes[static_cast<std::size_t>(selectedAction_-16)]=-1;
            RefreshGamepadProfile();StatusText(L"Controller binding cleared. Press Save or Play to keep the change.");return;
        }
        if(id==ResetPad) {
            CancelCapture(L"");Profile()=platform::NativeGamepadProfile{};RefreshGamepadProfile();
            StatusText(L"Selected player's controller profile reset to the original controls.");return;
        }
        CancelCapture(L"");
        if(id==KeyEscape || id==KeyDelete || id==KeyClear) {
            Keyboard()[static_cast<std::size_t>(selectedKey_)]=static_cast<std::uint16_t>(id==KeyEscape?VK_ESCAPE:id==KeyDelete?VK_DELETE:0);
            RefreshKeys();StatusText(L"Keyboard binding updated. Press Save or Play to keep it.");return;
        }
        if(id==ResetKeyboard) {
            const platform::NativeInputSettings defaults;
            Keyboard()=keyboardPlayer_==0?defaults.keys:defaults.player2Keys;RefreshKeys();
            StatusText(L"Selected keyboard profile reset. Press Save or Play to keep it.");return;
        }
        if(id==UnlockFps) {
            SetText(FrameLimit,L"0");CheckValue(Vsync,false);
            StatusText(L"Render FPS unlocked and VSync disabled. Output now depends on the selected graphics settings and hardware.");return;
        }
        if(id==RefreshMonitors) {RefreshMonitorsList();StatusText(L"Monitor list refreshed.");return;}
        if(id==RefreshDevices) {RefreshDevicesList(true);StatusText(L"Controller list refreshed.");return;}
        if(id==OriginalGraphics) {
            draft_.textureFilter=NativeTextureFilter::Authored;draft_.antialiasing=NativeAntialiasing::Off;
            draft_.anisotropy=16;draft_.render=NativeRenderSettings{};LoadGraphics();
            StatusText(L"Original graphics preset selected. Press Save or Play to keep it.");return;
        }
        if(id==KeyboardGamepad) {
            draft_.input.deviceMode={{platform::NativeDeviceMode::Keyboard,platform::NativeDeviceMode::Gamepad}};
            draft_.input.deviceKey[0].clear();draft_.input.controller[0]=-1;
            if(!devices_.empty()) {draft_.input.deviceKey[1]=devices_[0].id;draft_.input.controller[1]=devices_[0].index;}
            else {draft_.input.deviceKey[1].clear();draft_.input.controller[1]=0;}
            Select(ModeOne,1);Select(ModeTwo,2);RefreshDevicesList(true);
            StatusText(L"Keyboard: player 1. Gamepad: player 2. Select Players: 2 in the game's Challenge setup.");return;
        }
        if(id==IDCANCEL) {done_=true;DestroyWindow(window_);return;}
        if(id==Defaults) {
            draft_=NativePcSettings{};LoadControls();StatusText(L"Defaults selected. Press Save or Play to save them.");return;
        }
        if(id==OpenProfile) {
            std::filesystem::create_directories(launch_.profileDirectory);
            const auto opened=ShellExecuteW(window_,L"open",launch_.profileDirectory.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            Require(reinterpret_cast<INT_PTR>(opened)>32,"Could not open the profile folder");return;
        }
        if(id==Save || id==IDOK) {
            const auto chosen=ReadControls();
            if(id==IDOK && !launch_.configureOnly && !catalog_->Available() &&
                (platform::NativeGamepadEnabled(chosen.input,0) || platform::NativeGamepadEnabled(chosen.input,1)))
                throw std::runtime_error(catalog_->Error());
            saveNativePcSettings(file_,chosen);settings_=chosen;draft_=chosen;StatusText(L"Settings saved.");
            if(id==IDOK) {startGame_=!launch_.configureOnly;done_=true;DestroyWindow(window_);}
        }
    }
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM first,LPARAM second)
    {
        auto* self=reinterpret_cast<NativeConfigurator*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<NativeConfigurator*>(reinterpret_cast<CREATESTRUCTW*>(second)->lpCreateParams);
            self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(self)try {
            if(message==WM_CREATE) {self->CreateControls();return 0;}
            if(message==WM_COMMAND) {self->Command(LOWORD(first),HIWORD(first));return 0;}
            if(message==WM_TIMER && first==InputTimer) {self->Timer();return 0;}
            if(message==WM_SIZE && first!=SIZE_MINIMIZED && !self->loading_) {self->RedrawControls();return 0;}
            if(message==WM_DISPLAYCHANGE) {self->RefreshMonitorsList();return 0;}
            if(message==WM_ACTIVATEAPP && !first)self->CancelCapture(L"");
            if(message==DM_GETDEFID)return MAKELRESULT(IDOK,DC_HASDEFID);
            if(message==WM_NOTIFY && reinterpret_cast<NMHDR*>(second)->idFrom==Tab &&
                reinterpret_cast<NMHDR*>(second)->code==TCN_SELCHANGE) {
                self->ShowPage(static_cast<int>(SendDlgItemMessageW(window,Tab,TCM_GETCURSEL,0,0)));return 0;
            }
            if(message==WM_CLOSE) {self->done_=true;DestroyWindow(window);return 0;}
            if(message==WM_DESTROY) {KillTimer(window,InputTimer);self->window_=nullptr;self->done_=true;return 0;}
        } catch(const std::exception& error) {
            // WM_CREATE must return failure through Win32. Retain its original
            // exception so Run can report it once through the startup logger.
            if(message==WM_CREATE) {
                self->creationFailure_=std::current_exception();
                return -1;
            }
            if(message==WM_TIMER)KillTimer(window,InputTimer);
            MessageBoxW(window,NativeWideText(error.what()).c_str(),L"NFSHP2 - Settings",MB_OK|MB_ICONERROR);
            return 0;
        }
        return DefWindowProcW(window,message,first,second);
    }
    NativePcSettings& settings_;NativePcSettings draft_;const NativeProcessLaunch& launch_;std::filesystem::path file_;
    HWND window_{};HFONT font_{},headingFont_{};ATOM class_{};int dpi_{96};
    std::exception_ptr creationFailure_;
    std::array<std::vector<HWND>,6> pages_;
    std::vector<NativeWindowsMonitor> monitors_;std::vector<std::string> monitorKeys_;
    std::vector<platform::NativeInputDeviceInfo> devices_;std::array<std::vector<std::string>,2> deviceKeys_;
    std::unique_ptr<WindowsInputCatalog> catalog_;
    platform::NativePadSample captureBaseline_;
    int page_{},keyboardPlayer_{},gamepadPlayer_{},selectedKey_{14},selectedAction_{14},keyCapture_{-1},padCapture_{-1};
    ULONGLONG lastDiscovery_{},captureStarted_{},captureRested_{};
    bool loading_{true},done_{},startGame_{},captureArmed_{};
    bool axisDeadzoneDirty_{},axisSaturationDirty_{},axisInvertDirty_{},triggerDirty_{},vibrationDirty_{};
};
