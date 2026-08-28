// A native SwiftUI launcher for tvOS and iOS, replacing ge_launcher.cpp's ImGui UI on
// these two platforms only (macOS/Windows/Linux keep the existing desktop launcher).
//
// Why this exists: the ImGui launcher renders through SDL's own window/Metal-layer
// plumbing, which on iOS repeatedly produced a UI confined to a small fraction of the
// real screen -- traced through several real, fixed bugs (legacy 480x320 compatibility
// mode from a missing launch screen, a TV-oriented DisplaySize scale-down that was
// backwards on a screen smaller than its target, SDL_WINDOW_RESIZABLE silently widening
// the allowed orientation mask) without ever producing a UI that actually filled the
// screen on real hardware. SwiftUI has no equivalent failure mode: a `.ignoresSafeArea()`
// view fills its window by construction, on both platforms, at whatever the real screen
// size is, with taps/remote-focus handled by UIKit/tvOS's own hit-testing -- none of the
// coordinate-space bookkeeping ge_launcher.cpp has to do by hand for a custom ImGui
// backend. tvOS also gets the focus engine for free: a SwiftUI Button is Siri-Remote
// navigable with no extra code, unlike the touchpad-as-mouse emulation the ImGui path
// needed.
//
// Settings live in the SAME Model ge_launcher.cpp's own UI edits (GeLauncherBridge.h,
// SWIFT_OBJC_BRIDGING_HEADER) -- there is exactly one copy of "what mission, what
// ruleset, which mods are on", whichever UI wrote it last.
//
// C/Swift boundary: gePortNativeLauncherRun() is exposed with `@_cdecl`, which gives it
// the literal C symbol name with no Swift name-mangling -- ge_tvos_main.c calls it with a
// plain `extern int gePortNativeLauncherRun(void);` declaration, no bridging header
// needed for THAT direction (C calling into Swift). It matches gePortLauncherRun()'s own
// contract exactly: block until the user picks an action, then return 0 to fall through
// into the game in the same process (tvOS/iOS never execv() -- see relaunch()'s
// GE_PLATFORM_DESKTOP guard in ge_launcher.cpp) or return non-zero to stop.
#if !targetEnvironment(macCatalyst)
import SwiftUI
import UIKit

private let geBg = Color(red: 0.031, green: 0.035, blue: 0.043)
private let gePanel = Color(red: 0.075, green: 0.082, blue: 0.098)
private let geLine = Color(red: 0.16, green: 0.17, blue: 0.20)
private let geGold = Color(red: 0.86, green: 0.65, blue: 0.13)
private let geGoldHi = Color(red: 0.95, green: 0.78, blue: 0.25)
private let geText = Color(white: 0.92)
private let geDim = Color(white: 0.55)

// MARK: - Model, mirroring GeLauncherBridge.h's C surface as Swift state

private struct StageInfo: Identifiable {
    let id: Int
    let name: String
    let place: String
    let mission: Int
    let mpOnly: Bool
}

private struct ModInfo: Identifiable {
    let id: Int
    let name: String
}

private struct CheatInfo: Identifiable {
    let id: Int
    let label: String
    let live: Bool
}

private final class GeLauncherModel: ObservableObject {
    let stages: [StageInfo]
    let rulesets: [String]
    let mods: [ModInfo]
    let cheats: [CheatInfo]

    @Published var pickStage: Bool { didSet { geBridgeSetPickStage(pickStage ? 1 : 0) } }
    @Published var stageIdx: Int { didSet { geBridgeSetStageIdx(Int32(stageIdx)) } }
    @Published var profile: Int { didSet { geBridgeSetProfile(Int32(profile)) } }
    @Published var ruleset: Int { didSet { geBridgeSetRuleset(Int32(ruleset)) } }

    @Published var horde: Bool { didSet { geBridgeSetHorde(horde ? 1 : 0) } }
    @Published var hordePerKill: Int { didSet { geBridgeSetHordePerKill(Int32(hordePerKill)) } }
    @Published var hordePerKillCap: Int { didSet { geBridgeSetHordePerKillCap(Int32(hordePerKillCap)) } }
    @Published var hordeMaxAlive: Int { didSet { geBridgeSetHordeMaxAlive(Int32(hordeMaxAlive)) } }
    @Published var hordeWaveKills: Int { didSet { geBridgeSetHordeWaveKills(Int32(hordeWaveKills)) } }
    @Published var hordeGrowth: Int { didSet { geBridgeSetHordeGrowth(Int32(hordeGrowth)) } }

    @Published var supersample: Int { didSet { geBridgeSetSupersample(Int32(supersample)) } }
    @Published var fov: Int { didSet { geBridgeSetFov(Int32(fov)) } }
    @Published var framerate: Int { didSet { geBridgeSetFramerate(Int32(framerate)) } }
    @Published var msaa: Int { didSet { geBridgeSetMsaa(Int32(msaa)) } }
    @Published var fxaaOn: Bool { didSet { geBridgeSetFxaa(fxaaOn ? 1 : 0) } }
    @Published var hdTextures: Bool { didSet { geBridgeSetHdTextures(hdTextures ? 1 : 0) } }
    @Published var texpackPath: String { didSet { geBridgeSetTexpackPath(texpackPath) } }

    @Published var modOn: [Bool] {
        didSet { for i in modOn.indices { geBridgeSetModOn(Int32(i), modOn[i] ? 1 : 0) } }
    }
    @Published var cheatOn: [Bool] {
        didSet { for i in cheatOn.indices { geBridgeSetCheatOn(Int32(i), cheatOn[i] ? 1 : 0) } }
    }

    init() {
        geBridgeLoad()

        var st: [StageInfo] = []
        for i in 0..<Int(geBridgeStageCount()) {
            st.append(StageInfo(
                id: i,
                name: String(cString: geBridgeStageName(Int32(i))),
                place: String(cString: geBridgeStagePlace(Int32(i))),
                mission: Int(geBridgeStageMission(Int32(i))),
                mpOnly: geBridgeStageMpOnly(Int32(i)) != 0))
        }
        stages = st

        var rs: [String] = []
        for i in 0..<Int(geBridgeRulesetCount()) { rs.append(String(cString: geBridgeRulesetName(Int32(i)))) }
        rulesets = rs

        var md: [ModInfo] = []
        for i in 0..<Int(geBridgeModCount()) {
            md.append(ModInfo(id: i, name: String(cString: geBridgeModName(Int32(i)))))
        }
        mods = md

        var ch: [CheatInfo] = []
        for i in 0..<Int(geBridgeCheatCount()) {
            ch.append(CheatInfo(id: i, label: String(cString: geBridgeCheatLabel(Int32(i))),
                                 live: geBridgeCheatLive(Int32(i)) != 0))
        }
        cheats = ch

        pickStage = geBridgeGetPickStage() != 0
        stageIdx = Int(geBridgeGetStageIdx())
        profile = Int(geBridgeGetProfile())
        ruleset = Int(geBridgeGetRuleset())
        horde = geBridgeGetHorde() != 0
        hordePerKill = Int(geBridgeGetHordePerKill())
        hordePerKillCap = Int(geBridgeGetHordePerKillCap())
        hordeMaxAlive = Int(geBridgeGetHordeMaxAlive())
        hordeWaveKills = Int(geBridgeGetHordeWaveKills())
        hordeGrowth = Int(geBridgeGetHordeGrowth())
        supersample = Int(geBridgeGetSupersample())
        fov = Int(geBridgeGetFov())
        framerate = Int(geBridgeGetFramerate())
        msaa = Int(geBridgeGetMsaa())
        fxaaOn = geBridgeGetFxaa() != 0
        hdTextures = geBridgeGetHdTextures() != 0
        texpackPath = String(cString: geBridgeGetTexpackPath())
        modOn = (0..<md.count).map { geBridgeGetModOn(Int32($0)) != 0 }
        cheatOn = (0..<ch.count).map { geBridgeGetCheatOn(Int32($0)) != 0 }
    }

    func save() { geBridgeSave() }
}

// MARK: - Shared bits

private struct GePanel<Content: View>: View {
    let content: Content
    init(@ViewBuilder content: () -> Content) { self.content = content() }
    var body: some View {
        content
            .padding(20)
            .background(gePanel)
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(geLine, lineWidth: 1))
    }
}

private struct GeSectionTitle: View {
    let text: String
    var body: some View {
        Text(text.uppercased())
            .font(.system(size: 13, weight: .bold))
            .tracking(2)
            .foregroundColor(geDim)
    }
}

private struct GeButtonStyle: ButtonStyle {
    var primary: Bool = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .opacity(configuration.isPressed ? 0.8 : 1.0)
            .scaleEffect(configuration.isPressed ? 0.98 : 1.0)
    }
}

/* Plain buttons, not SwiftUI's Stepper: Stepper's whole control type is unavailable on
 * tvOS (no equivalent of a precise, small +/- tap target with a remote), where a Button
 * is what the focus engine navigates between either way. */
private struct GeStepper: View {
    let label: String
    @Binding var value: Int
    let range: ClosedRange<Int>
    var step: Int = 1
    var suffix: String = ""

    var body: some View {
        HStack {
            Text(label).foregroundColor(geText).font(.system(size: 15))
            Spacer()
            Text("\(value)\(suffix)").foregroundColor(geGoldHi).font(.system(size: 15, weight: .semibold))
                .frame(minWidth: 60, alignment: .trailing)
            Button(action: { value = max(range.lowerBound, value - step) }) {
                Text("-").font(.system(size: 18, weight: .bold)).foregroundColor(geText)
                    .frame(width: 36, height: 30).background(gePanel)
                    .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
            }
            .buttonStyle(GeButtonStyle())
            Button(action: { value = min(range.upperBound, value + step) }) {
                Text("+").font(.system(size: 18, weight: .bold)).foregroundColor(geText)
                    .frame(width: 36, height: 30).background(gePanel)
                    .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
            }
            .buttonStyle(GeButtonStyle())
        }
    }
}

// MARK: - Pages

private struct MissionPage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Toggle(isOn: $m.pickStage) {
                Text("Start on a specific mission").foregroundColor(geText)
            }

            if m.pickStage {
                ScrollView {
                    LazyVStack(spacing: 6) {
                        ForEach(m.stages) { s in
                            Button(action: { m.stageIdx = s.id }) {
                                HStack {
                                    Text(s.mission > 0 ? String(format: "%02d", s.mission) : "MP")
                                        .font(.system(size: 12, weight: .bold))
                                        .foregroundColor(m.stageIdx == s.id ? geGoldHi : geGold)
                                        .frame(width: 34, alignment: .leading)
                                    Text(s.name).foregroundColor(geText).font(.system(size: 16))
                                    Spacer()
                                    Text(s.place).foregroundColor(geDim).font(.system(size: 13))
                                }
                                .padding(.horizontal, 14).padding(.vertical, 10)
                                .background(m.stageIdx == s.id ? geGold.opacity(0.22) : gePanel)
                            }
                            .buttonStyle(GeButtonStyle())
                        }
                    }
                }
            } else {
                Text("The game boots to the title screen and the mission is chosen there.")
                    .foregroundColor(geDim).font(.system(size: 14))
                Spacer()
            }
        }
    }
}

private struct RulesetPage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            GeSectionTitle(text: "Ruleset")
            LazyVStack(spacing: 6) {
                ForEach(m.rulesets.indices, id: \.self) { i in
                    Button(action: { m.ruleset = i }) {
                        HStack {
                            Text(m.rulesets[i].capitalized).foregroundColor(geText).font(.system(size: 16))
                            Spacer()
                            if m.ruleset == i {
                                Image(systemName: "checkmark").foregroundColor(geGoldHi)
                            }
                        }
                        .padding(.horizontal, 14).padding(.vertical, 10)
                        .background(m.ruleset == i ? geGold.opacity(0.22) : gePanel)
                    }
                    .buttonStyle(GeButtonStyle())
                }
            }
            Spacer()
        }
    }
}

private struct HordePage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Toggle(isOn: $m.horde) { Text("Horde mode").foregroundColor(geText) }
            if m.horde {
                GePanel {
                    VStack(spacing: 14) {
                        GeStepper(label: "Spawns per kill", value: $m.hordePerKill, range: 0...8)
                        GeStepper(label: "Spawns per kill (cap)", value: $m.hordePerKillCap, range: 1...20)
                        GeStepper(label: "Max alive", value: $m.hordeMaxAlive, range: 1...64)
                        GeStepper(label: "Wave kills", value: $m.hordeWaveKills, range: 1...100)
                        GeStepper(label: "Growth", value: $m.hordeGrowth, range: 0...10)
                    }
                }
            }
            Spacer()
        }
    }
}

private struct ModsPage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            GeSectionTitle(text: m.mods.isEmpty ? "Mods (none found)" : "Mods")
            ScrollView {
                LazyVStack(spacing: 6) {
                    ForEach(m.mods) { mod in
                        Toggle(isOn: Binding(
                            get: { m.modOn[mod.id] },
                            set: { m.modOn[mod.id] = $0 }
                        )) {
                            Text(mod.name).foregroundColor(geText).font(.system(size: 15))
                        }
                        .padding(.horizontal, 14).padding(.vertical, 8)
                        .background(gePanel)
                    }
                }
            }
        }
    }
}

private struct CheatsPage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            GeSectionTitle(text: "Cheats")
            ScrollView {
                LazyVStack(spacing: 6) {
                    ForEach(m.cheats) { c in
                        Toggle(isOn: Binding(
                            get: { m.cheatOn[c.id] },
                            set: { m.cheatOn[c.id] = $0 }
                        )) {
                            HStack {
                                Text(c.label).foregroundColor(geText).font(.system(size: 15))
                                if !c.live {
                                    Text("(needs in-game activation)")
                                        .foregroundColor(geDim).font(.system(size: 11))
                                }
                            }
                        }
                        .padding(.horizontal, 14).padding(.vertical, 8)
                        .background(gePanel)
                    }
                }
            }
        }
    }
}

private struct VideoPage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            GeSectionTitle(text: "Video")
            GePanel {
                /* Supersample, MSAA and FXAA are NOT here: gfx_metal.mm's own header
                 * comment lists supersample/CRT/FXAA post-processing as unimplemented
                 * "KNOWN v1 GAPS" on this renderer -- the game's shared gfx_pc.c never
                 * even sets gfx_supersample above 1 under RAPI_METAL (only
                 * gfx_opengl.c's now-inert-here code path does), and GETV_MSAA is read
                 * inside an explicit #ifndef RAPI_METAL block in gfx_sdl2.c. tvOS and
                 * iOS are both always Metal here, so exposing sliders for settings that
                 * silently do nothing would just be misleading -- confirmed by directly
                 * testing GETV_SUPERSAMPLE=2/GETV_MSAA=4/GETV_ANISO=8 against a real
                 * level load on the iOS Simulator: identical triangle counts and
                 * geometry to the same run without them. Field of view and the frame
                 * rate cap are ordinary gameplay/pacing settings with no renderer
                 * dependency, so they stay. */
                VStack(spacing: 14) {
                    GeStepper(label: "Field of view", value: $m.fov, range: 60...140, step: 5)
                    GeStepper(label: "Frame rate cap", value: $m.framerate, range: 30...60, step: 30, suffix: " fps")
                }
            }

            /* Unlike supersample/MSAA/FXAA above, HD textures go through gfx_pc.c's
             * backend-agnostic ge_texpack_try_override() -> GfxRenderingAPI.upload_texture,
             * which gfx_metal.mm implements fully -- this is real on tvOS/iOS today, not a
             * renderer gap, so it belongs on this page regardless of backend. */
            GeSectionTitle(text: "HD Textures")
            GePanel {
                VStack(alignment: .leading, spacing: 14) {
                    Toggle(isOn: $m.hdTextures) {
                        Text("Load HD texture pack").foregroundColor(geText)
                    }
                    if m.hdTextures {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("Pack folder").foregroundColor(geDim).font(.system(size: 13))
                            TextField("hdtextures", text: $m.texpackPath)
                                .foregroundColor(geText)
                                .padding(10)
                                .background(gePanel)
                                .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
                        }
                    }
                }
            }
            Spacer()
        }
    }
}

private struct ProfilePage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            GeSectionTitle(text: "Profile")
            LazyVStack(spacing: 6) {
                ForEach([(0, "97 Console", "The game as shipped."),
                         (1, "GoldenEye+", "This port's own enhancements: higher supersample, MSAA, anisotropic filtering, FOV.")], id: \.0) { p in
                    Button(action: { m.profile = p.0 }) {
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Text(p.1).foregroundColor(geText).font(.system(size: 16, weight: .semibold))
                                Spacer()
                                if m.profile == p.0 { Image(systemName: "checkmark").foregroundColor(geGoldHi) }
                            }
                            Text(p.2).foregroundColor(geDim).font(.system(size: 12))
                        }
                        .padding(.horizontal, 14).padding(.vertical, 10)
                        .background(m.profile == p.0 ? geGold.opacity(0.22) : gePanel)
                    }
                    .buttonStyle(GeButtonStyle())
                }
            }
            Spacer()
        }
    }
}

// MARK: - Root

private enum GePage: Int, CaseIterable {
    case mission, ruleset, horde, mods, cheats, video, profile
    var title: String {
        switch self {
        case .mission: return "Mission"
        case .ruleset: return "Ruleset"
        case .horde: return "Horde"
        case .mods: return "Mods"
        case .cheats: return "Cheats"
        case .video: return "Video"
        case .profile: return "Profile"
        }
    }
}

private struct GeLauncherView: View {
    @StateObject private var m = GeLauncherModel()
    @State private var page: GePage = .mission
    let onStart: () -> Void

    /* A fixed constant, not a measured safe-area inset: the outer .ignoresSafeArea() below
     * is doing real work (see its own comment: without it the footer's Start Mission button
     * gets squeezed off the bottom on a real iPhone), and once an ancestor ignores safe
     * area, SwiftUI reports a ZERO safe area to every descendant that tries to read it back
     * -- GeometryReader.safeAreaInsets included, even placed on the exact same view as the
     * ignoresSafeArea() call. Measured empirically across three different approaches
     * (.safeAreaPadding on a descendant, GeometryReader nested inside ignoresSafeArea,
     * GeometryReader wrapped BY ignoresSafeArea) -- all three read zero. Apple's own
     * Dynamic Island footprint is a fixed, documented size (roughly 126x37.33pt in
     * portrait), so a fixed clearance is not a worse approximation than a "measurement"
     * that reads zero; it is a better one. Harmless on a device with no island: a few
     * points of unused left margin on the nav rail, not a visual defect.
     */
    private let leadingInset: CGFloat = 40

    var body: some View {
        content(leadingInset: leadingInset)
            .ignoresSafeArea()
    }

    private func content(leadingInset: CGFloat) -> some View {
        ZStack {
            geBg

            VStack(spacing: 0) {
                // Header
                HStack {
                    Text("GOLDENEYE 007")
                        .font(.system(size: 22, weight: .black))
                        .tracking(4)
                        .foregroundColor(geGold)
                    Spacer()
                    Text("NATIVE PORT")
                        .font(.system(size: 11, weight: .semibold))
                        .tracking(3)
                        .foregroundColor(geDim)
                }
                .padding(.horizontal, 24).padding(.vertical, 10)
                .background(gePanel)

                HStack(spacing: 0) {
                    // Nav rail. Scrollable, not a bare VStack: seven pages at any
                    // reasonable padding overflow an iPhone's landscape height (as low
                    // as ~360pt) even after the safe-area fix below -- a plain VStack
                    // does not scroll on overflow, it just clips silently.
                    //
                    // leadingInset padding below: this HStack sits in the screen's
                    // vertical middle band, exactly where a Dynamic Island lands as a
                    // LEADING inset once forced landscape (it starts life as a top-center
                    // notch in portrait) -- verified by screenshot, the island was
                    // overlapping the MODS/CHEATS tab labels without this. leadingInset
                    // comes from the body's own GeometryReader (see its comment for why
                    // that, not .safeAreaPadding, is what actually works here).
                    ScrollView(showsIndicators: false) {
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(GePage.allCases, id: \.self) { p in
                                Button(action: { page = p }) {
                                    Text(p.title.uppercased())
                                        .font(.system(size: 13, weight: page == p ? .bold : .regular))
                                        .tracking(1)
                                        .foregroundColor(page == p ? geGoldHi : geDim)
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                        .padding(.horizontal, 16).padding(.vertical, 9)
                                        .background(page == p ? geGold.opacity(0.15) : Color.clear)
                                }
                                .buttonStyle(GeButtonStyle())
                            }
                        }
                    }
                    .frame(width: 180)
                    .background(gePanel.opacity(0.5))

                    // Page content
                    Group {
                        switch page {
                        case .mission: MissionPage(m: m)
                        case .ruleset: RulesetPage(m: m)
                        case .horde: HordePage(m: m)
                        case .mods: ModsPage(m: m)
                        case .cheats: CheatsPage(m: m)
                        case .video: VideoPage(m: m)
                        case .profile: ProfilePage(m: m)
                        }
                    }
                    .padding(16)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                }
                .padding(.leading, leadingInset)

                // Footer
                HStack {
                    Spacer()
                    Button(action: {
                        m.save()
                        onStart()
                    }) {
                        Text("START MISSION")
                            .font(.system(size: 16, weight: .bold))
                            .tracking(3)
                            .foregroundColor(Color(white: 0.05))
                            .padding(.horizontal, 32)
                            .padding(.vertical, 10)
                            .background(geGold)
                    }
                    .buttonStyle(GeButtonStyle())
                }
                .padding(.horizontal, 24).padding(.vertical, 10)
                .background(gePanel)
            }
        }
    }
}

// MARK: - Bridge to ge_tvos_main.c

private final class GeLauncherBridgeRunner {
    static let shared = GeLauncherBridgeRunner()
    private var window: UIWindow?
    private var finished = false

    func run() -> Int32 {
        // GETV_LAUNCHER_AUTOPLAY=1: skip presenting the window and fall straight through
        // to the game with whatever GETV_* settings are already in the environment (e.g.
        // GETV_STAGE). Mirrors the old ImGui launcher's own GETV_LAUNCHER_AUTOPLAY -- it
        // exists for the same reason: automated boots (headless local runs, CI)
        // have nothing to tap a SwiftUI button with. model_load()+model_store() still
        // run via geBridgeLoad()/geBridgeSave(), so a value set only through the
        // launcher's own env-var reading (not a live model edit) is preserved exactly
        // like a real "Start Mission" tap would leave it.
        if ProcessInfo.processInfo.environment["GETV_LAUNCHER_AUTOPLAY"] == "1" {
            geBridgeLoad()
            geBridgeSave()
            return 0
        }

        // No existing window to present onto, on either platform: this runs from
        // SDL_main(), which is reached almost immediately after UIApplicationMain()'s own
        // bootstrap and well before SDL creates the GAME's window (that happens later, in
        // gfx_sdl2.c) -- so there is nothing to find yet. Verified on the real Apple TV:
        // UIApplication.shared.connectedScenes.first's window list is empty at this
        // point. Owning the window outright sidesteps that entirely -- no dependency on
        // SDL's own window lifecycle at all.
        let newWindow: UIWindow
        if let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene }).first {
            newWindow = UIWindow(windowScene: scene)
        } else {
            newWindow = UIWindow(frame: UIScreen.main.bounds)
        }

        finished = false
        let view = GeLauncherView(onStart: { [weak self] in self?.finished = true })
        let vc = UIHostingController(rootView: view)
        vc.view.backgroundColor = .black
        newWindow.rootViewController = vc
        newWindow.windowLevel = .normal
        newWindow.makeKeyAndVisible()
        window = newWindow

        // No-op on tvOS (already landscape-only, no orientation to force). On iOS this is
        // the same fix gfx_sdl2.c applies to the GAME window -- the launcher has its own,
        // earlier UIWindow that needs it independently, or it launches sideways in
        // whatever orientation the device happened to be in.
        gePortForceLandscapeOrientation()

        // Matters most on tvOS, but harmless (and still correct) on iOS too, which has
        // its own focus system for external keyboards/game controllers: a window built
        // and made key outside the normal UIWindowScene delegate flow -- which this one
        // is, on purpose, since nothing exists yet to attach to (see the comment above)
        // -- does not automatically get an initial focused element the way a
        // scene-delegate-owned window does. With nothing focused, the focus engine has
        // no "current position" to move away from, so Siri Remote swipes and a
        // connected game controller's D-pad both look completely dead even though the
        // UI is visibly on screen and its buttons ARE focusable. Forcing one focus pass
        // here seeds it.
        newWindow.setNeedsFocusUpdate()
        newWindow.updateFocusIfNeeded()

        // A nested run loop: gePortLauncherRun()'s C++ implementation blocks its caller
        // synchronously until the user acts, and ge_tvos_main.c's boot sequence is written
        // against that contract on both platforms already -- this is the standard,
        // long-used technique for making a UIKit-driven, event-based interaction look
        // synchronous to a caller that cannot itself be restructured into a callback.
        while !finished {
            CFRunLoopRunInMode(.defaultMode, 0.05, true)
        }

        // Torn down completely, not just hidden: the game creates its OWN UIWindow next
        // (SDL_CreateWindow(), later in gfx_sdl2.c), and a leftover key window here would
        // compete with it for key/visible status exactly like the single-window conflicts
        // already worked through elsewhere on this port (see ge_tvos_main.c's boot-order
        // comment on why tvOS/iOS never execv() between launcher and game).
        window?.isHidden = true
        window?.rootViewController = nil
        window = nil
        return 0
    }
}

@_cdecl("gePortNativeLauncherRun")
public func gePortNativeLauncherRun() -> Int32 {
    return GeLauncherBridgeRunner.shared.run()
}
#endif
