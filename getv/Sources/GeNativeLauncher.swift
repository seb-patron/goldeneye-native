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
#if os(macOS)
import AppKit
#else
import UIKit
#endif
import GameController

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
    // var, not let: RESCAN (geBridgeRescanMods -> mod_scan) can discover new folders or lose
    // deleted ones, not just flip existing entries' on/off state -- unlike stages/rulesets/
    // cheats, which are fixed for the process's whole lifetime.
    @Published var mods: [ModInfo]
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

    @Published var aniso: Int { didSet { geBridgeSetAniso(Int32(aniso)) } }
    @Published var filtering: Int { didSet { geBridgeSetFiltering(Int32(filtering)) } }
    @Published var widescreen: Bool { didSet { geBridgeSetWidescreen(widescreen ? 1 : 0) } }
    @Published var mipmaps: Bool { didSet { geBridgeSetMipmaps(mipmaps ? 1 : 0) } }
    @Published var parallax: Bool { didSet { geBridgeSetParallax(parallax ? 1 : 0) } }
    @Published var crosshairScalePct: Int { didSet { geBridgeSetCrosshairScalePct(Int32(crosshairScalePct)) } }
    // 0...100, not 0...1 -- matches every other percentage-styled GeStepper on this page
    // rather than introducing a differently-scaled control just for this one row.
    @Published var crosshairRPct: Int { didSet { pushCrosshairColor() } }
    @Published var crosshairGPct: Int { didSet { pushCrosshairColor() } }
    @Published var crosshairBPct: Int { didSet { pushCrosshairColor() } }
    private func pushCrosshairColor() {
        geBridgeSetCrosshairColor(Float(crosshairRPct) / 100.0, Float(crosshairGPct) / 100.0,
                                   Float(crosshairBPct) / 100.0)
    }
    @Published var fullscreen: Bool { didSet { geBridgeSetFullscreen(fullscreen ? 1 : 0) } }
    @Published var resolution: String { didSet { geBridgeSetResolution(resolution) } }
    @Published var uncapped: Bool { didSet { geBridgeSetUncapped(uncapped ? 1 : 0) } }
    @Published var devOverlay: Bool { didSet { geBridgeSetDevOverlay(devOverlay ? 1 : 0) } }

    @Published var rsCustom: Bool { didSet { geBridgeSetRsCustom(rsCustom ? 1 : 0) } }
    @Published var enemyHealth: Int { didSet { geBridgeSetEnemyHealth(Int32(enemyHealth)) } }
    @Published var enemyDamage: Int { didSet { geBridgeSetEnemyDamage(Int32(enemyDamage)) } }
    @Published var enemyAccuracy: Int { didSet { geBridgeSetEnemyAccuracy(Int32(enemyAccuracy)) } }
    @Published var enemyReaction: Int { didSet { geBridgeSetEnemyReaction(Int32(enemyReaction)) } }
    @Published var playerHealth: Int { didSet { geBridgeSetPlayerHealth(Int32(playerHealth)) } }
    @Published var playerArmour: Int { didSet { geBridgeSetPlayerArmour(Int32(playerArmour)) } }
    @Published var ammoPct: Int { didSet { geBridgeSetAmmoPct(Int32(ammoPct)) } }
    @Published var explosionDamage: Int { didSet { geBridgeSetExplosionDamage(Int32(explosionDamage)) } }
    @Published var turretDamage: Int { didSet { geBridgeSetTurretDamage(Int32(turretDamage)) } }

    @Published var mouse: Bool { didSet { geBridgeSetMouse(mouse ? 1 : 0) } }
    @Published var mouseSens: Int { didSet { geBridgeSetMouseSens(Int32(mouseSens)) } }
    @Published var mouseInvert: Bool { didSet { geBridgeSetMouseInvert(mouseInvert ? 1 : 0) } }
    @Published var keyboard: Bool { didSet { geBridgeSetKeyboard(keyboard ? 1 : 0) } }

    @Published var modDir: String { didSet { geBridgeSetModDir(modDir) } }

    // Control bindings: 6 actions x (ALL + 4 players) is a small get/set surface better
    // served by two methods than by 30 separate @Published fields -- objectWillChange is
    // sent by hand since these bypass the @Published property wrapper entirely.
    @Published var bindTab: Int { didSet { geBridgeSetBindTab(Int32(bindTab)) } }
    let actions: [(label: String, dflt: String)]
    let sources: [String]

    func bindSlot(action: Int) -> Int {
        bindTab == 0 ? Int(geBridgeGetBindAll(Int32(action)))
                      : Int(geBridgeGetBindP(Int32(bindTab - 1), Int32(action)))
    }
    /* What this action effectively does if nothing more specific is chosen: the ALL tab's
     * value when one exists, otherwise the action's own built-in default -- mirrors
     * ge_launcher.cpp's ImGui page exactly, including on the ALL tab itself, where "eff"
     * and "default" coincide. */
    func effectiveBindLabel(action: Int) -> String {
        let all = Int(geBridgeGetBindAll(Int32(action)))
        return all >= 0 ? sources[all] : actions[action].dflt
    }
    func setBindSlot(action: Int, src: Int) {
        objectWillChange.send()
        if bindTab == 0 { geBridgeSetBindAll(Int32(action), Int32(src)) }
        else { geBridgeSetBindP(Int32(bindTab - 1), Int32(action), Int32(src)) }
    }
    func resetBindTab() {
        objectWillChange.send()
        geBridgeResetBindTab()
    }

    @Published var modOn: [Bool] {
        didSet { for i in modOn.indices { geBridgeSetModOn(Int32(i), modOn[i] ? 1 : 0) } }
    }
    @Published var cheatOn: [Bool] {
        didSet { for i in cheatOn.indices { geBridgeSetCheatOn(Int32(i), cheatOn[i] ? 1 : 0) } }
    }

    func rescanMods() {
        geBridgeRescanMods()
        var md: [ModInfo] = []
        for i in 0..<Int(geBridgeModCount()) {
            md.append(ModInfo(id: i, name: String(cString: geBridgeModName(Int32(i)))))
        }
        mods = md
        modOn = (0..<md.count).map { geBridgeGetModOn(Int32($0)) != 0 }
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

        var ac: [(label: String, dflt: String)] = []
        for i in 0..<Int(geBridgeActionCount()) {
            ac.append((label: String(cString: geBridgeActionLabel(Int32(i))),
                       dflt: String(cString: geBridgeActionDefault(Int32(i)))))
        }
        actions = ac
        var sc: [String] = []
        for i in 0..<Int(geBridgeSourceCount()) { sc.append(String(cString: geBridgeSourceName(Int32(i)))) }
        sources = sc

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

        aniso = Int(geBridgeGetAniso())
        filtering = Int(geBridgeGetFiltering())
        widescreen = geBridgeGetWidescreen() != 0
        mipmaps = geBridgeGetMipmaps() != 0
        parallax = geBridgeGetParallax() != 0
        crosshairScalePct = Int(geBridgeGetCrosshairScalePct())
        crosshairRPct = Int((geBridgeGetCrosshairR() * 100.0).rounded())
        crosshairGPct = Int((geBridgeGetCrosshairG() * 100.0).rounded())
        crosshairBPct = Int((geBridgeGetCrosshairB() * 100.0).rounded())
        fullscreen = geBridgeGetFullscreen() != 0
        resolution = String(cString: geBridgeGetResolution())
        uncapped = geBridgeGetUncapped() != 0
        devOverlay = geBridgeGetDevOverlay() != 0

        rsCustom = geBridgeGetRsCustom() != 0
        enemyHealth = Int(geBridgeGetEnemyHealth())
        enemyDamage = Int(geBridgeGetEnemyDamage())
        enemyAccuracy = Int(geBridgeGetEnemyAccuracy())
        enemyReaction = Int(geBridgeGetEnemyReaction())
        playerHealth = Int(geBridgeGetPlayerHealth())
        playerArmour = Int(geBridgeGetPlayerArmour())
        ammoPct = Int(geBridgeGetAmmoPct())
        explosionDamage = Int(geBridgeGetExplosionDamage())
        turretDamage = Int(geBridgeGetTurretDamage())

        mouse = geBridgeGetMouse() != 0
        mouseSens = Int(geBridgeGetMouseSens())
        mouseInvert = geBridgeGetMouseInvert() != 0
        keyboard = geBridgeGetKeyboard() != 0

        modDir = String(cString: geBridgeGetModDir())
        bindTab = Int(geBridgeGetBindTab())

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

/* A custom ButtonStyle REPLACES tvOS's automatic focus "pop" halo entirely -- the system
 * only applies that for the default/.plain style, so a style that doesn't render its own
 * focus state makes every button on this screen look focus-dead under Siri Remote/game
 * controller navigation even though the focus engine is moving normally underneath. The
 * fix has to live in a child view, not read directly off `configuration` here: SwiftUI
 * only resolves @Environment(\.isFocused) correctly when it's read inside the view that is
 * itself the focusable element's body, not one level up in the style function. */
private struct GeButtonStyle: ButtonStyle {
    var primary: Bool = false
    func makeBody(configuration: Configuration) -> some View {
        GeButtonLabel(configuration: configuration)
    }
}

private struct GeButtonLabel: View {
    let configuration: GeButtonStyle.Configuration
    @Environment(\.isFocused) private var isFocused
    var body: some View {
        configuration.label
            .opacity(configuration.isPressed ? 0.8 : 1.0)
            .scaleEffect(isFocused ? 1.06 : (configuration.isPressed ? 0.98 : 1.0))
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(isFocused ? geGoldHi : Color.clear, lineWidth: 3)
            )
            .shadow(color: isFocused ? geGoldHi.opacity(0.55) : .clear, radius: isFocused ? 12 : 0)
            .animation(.easeOut(duration: 0.15), value: isFocused)
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
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                GeSectionTitle(text: "Ruleset")
                LazyVStack(spacing: 6) {
                    ForEach(m.rulesets.indices, id: \.self) { i in
                        Button(action: { m.ruleset = i }) {
                            HStack {
                                Text(m.rulesets[i].capitalized).foregroundColor(geText).font(.system(size: 16))
                                Spacer()
                                if m.ruleset == i && !m.rsCustom {
                                    Image(systemName: "checkmark").foregroundColor(geGoldHi)
                                }
                            }
                            .padding(.horizontal, 14).padding(.vertical, 10)
                            .background(m.ruleset == i && !m.rsCustom ? geGold.opacity(0.22) : gePanel)
                        }
                        .buttonStyle(GeButtonStyle())
                    }
                }

                Toggle(isOn: $m.rsCustom) {
                    Text("Override with custom values").foregroundColor(geText)
                }
                .padding(.top, 4)

                if m.rsCustom {
                    Text("Percentages of the original. 100 is unmodified. These replace the preset above.")
                        .foregroundColor(geDim).font(.system(size: 12))
                    GePanel {
                        VStack(spacing: 14) {
                            GeStepper(label: "Enemy health", value: $m.enemyHealth, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Enemy damage", value: $m.enemyDamage, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Enemy accuracy", value: $m.enemyAccuracy, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Enemy reaction", value: $m.enemyReaction, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Player health", value: $m.playerHealth, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Player armour", value: $m.playerArmour, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Ammo", value: $m.ammoPct, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Explosions", value: $m.explosionDamage, range: 10...500, step: 10, suffix: "%")
                            GeStepper(label: "Turrets", value: $m.turretDamage, range: 10...500, step: 10, suffix: "%")
                        }
                    }
                }
                Spacer()
            }
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

private struct ControlBindRow: View {
    @ObservedObject var m: GeLauncherModel
    let action: Int

    var body: some View {
        let slot = m.bindSlot(action: action)
        let unsetLabel = m.bindTab == 0
            ? "default (\(m.actions[action].dflt))"
            : "same as all (\(m.effectiveBindLabel(action: action)))"
        let previewLabel = slot >= 0 ? m.sources[slot] : unsetLabel

        HStack {
            Text(m.actions[action].label).foregroundColor(geText).font(.system(size: 15))
            Spacer()
            Menu {
                Button(unsetLabel) { m.setBindSlot(action: action, src: -1) }
                Divider()
                ForEach(m.sources.indices, id: \.self) { s in
                    Button(m.sources[s]) { m.setBindSlot(action: action, src: s) }
                }
            } label: {
                Text(previewLabel)
                    .font(.system(size: 14))
                    .foregroundColor(geGoldHi)
                    .padding(.horizontal, 12).padding(.vertical, 8)
                    .background(gePanel)
                    .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
            }
        }
        .padding(.vertical, 4)
    }
}

private struct ControlsPage: View {
    @ObservedObject var m: GeLauncherModel

    /* Fixed, not rebindable (port_input.c's keyboard map has no remap layer) -- shown as a
     * reference rather than a control, mirroring ge_launcher.cpp's own ImGui page: the
     * campaign was unfinishable from the keyboard until USE existed and nothing on screen
     * said which key that was. */
    static let keyboardReference: [(String, String)] = [
        ("W A S D", "move"),
        ("Arrow keys", "look"),
        ("Space / L-Ctrl", "fire"),
        ("Q", "aim"),
        ("E or F", "use"),
        ("R or Return", "inventory"),
        ("Z / X", "crouch (L / R)"),
        ("I J K L", "d-pad"),
        ("Tab", "start"),
    ]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                mouseKeyboardSection
                bindingsHeaderSection
                actionsSection
                footerSection
            }
        }
    }

    /* body is split into these four sections -- and each of THOSE stays a handful of
     * children -- rather than one flat 12-child VStack: SwiftUI's @ViewBuilder resolves a
     * block above its built-in child-count ceiling into `EmptyView` in some toolchain
     * configurations, with NO compile error and NO runtime crash, just silently missing
     * content. Measured directly on this machine: the six action rows below simply did not
     * appear -- confirmed via a temporary child-count debug label, then confirmed again by
     * swapping the real Menu-based row for a plain Text (ruling out Menu as the cause) --
     * until the body was split like this. */
    private var mouseKeyboardSection: some View {
        Group {
            GeSectionTitle(text: "Mouse and Keyboard")
            GePanel {
                VStack(alignment: .leading, spacing: 10) {
                    Toggle(isOn: $m.mouse) { Text("Mouse look").foregroundColor(geText) }
                    if m.mouse {
                        GeStepper(label: "Sensitivity", value: $m.mouseSens, range: 10...400, suffix: "%")
                        Toggle(isOn: $m.mouseInvert) { Text("Invert Y").foregroundColor(geText) }
                    }
                }
            }
            GePanel {
                VStack(alignment: .leading, spacing: 10) {
                    Toggle(isOn: $m.keyboard) { Text("Keyboard").foregroundColor(geText) }
                    if m.keyboard {
                        VStack(alignment: .leading, spacing: 6) {
                            ForEach(Array(ControlsPage.keyboardReference.enumerated()), id: \.offset) { _, row in
                                HStack {
                                    Text(row.0).foregroundColor(geGold).font(.system(size: 13, weight: .semibold))
                                        .frame(width: 140, alignment: .leading)
                                    Text(row.1).foregroundColor(geDim).font(.system(size: 13))
                                }
                            }
                        }
                        Text("Fixed, not rebindable -- a key is indistinguishable from a thumb on a stick by the time the game sees it.")
                            .foregroundColor(geDim).font(.system(size: 11))
                    }
                }
            }
        }
    }

    private var bindingsHeaderSection: some View {
        Group {
            GeSectionTitle(text: "Bindings For")
            HStack(spacing: 6) {
                ForEach(Array(["ALL", "P1", "P2", "P3", "P4"].enumerated()), id: \.offset) { i, label in
                    Button(action: { m.bindTab = i }) {
                        Text(label)
                            .font(.system(size: 13, weight: m.bindTab == i ? .bold : .regular))
                            .foregroundColor(m.bindTab == i ? geGoldHi : geDim)
                            .padding(.horizontal, 16).padding(.vertical, 8)
                            .background(m.bindTab == i ? geGold.opacity(0.15) : gePanel)
                    }
                    .buttonStyle(GeButtonStyle())
                }
            }
            Text(m.bindTab == 0
                 ? "Applies to every player. A player with its own choice below overrides this one."
                 : "Applies to this player only. Anything left on \"same as all\" follows the ALL tab.")
                .foregroundColor(geDim).font(.system(size: 12))
        }
    }

    private var actionsSection: some View {
        Group {
            GeSectionTitle(text: "Actions")
            GePanel {
                VStack(spacing: 4) {
                    ForEach(m.actions.indices, id: \.self) { a in
                        ControlBindRow(m: m, action: a)
                    }
                }
            }
        }
    }

    private var footerSection: some View {
        Group {
            Button(action: { m.resetBindTab() }) {
                Text("RESET THIS TAB")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(geText)
                    .padding(.horizontal, 16).padding(.vertical, 10)
                    .background(gePanel)
                    .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
            }
            .buttonStyle(GeButtonStyle())

            Text("Button names are positional, not printed labels. \"a\" is always the bottom face button, including on Nintendo pads where it is marked B.")
                .foregroundColor(geDim).font(.system(size: 12))
            Text("Crouch is deliberately absent -- in the two-controller styles it is controller 2's stick Y crossing +/-30 while aiming, not a button.")
                .foregroundColor(geDim).font(.system(size: 12))
            Spacer()
        }
    }
}

private struct ModsPage: View {
    @ObservedObject var m: GeLauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            GeSectionTitle(text: "Mod directory")
            GePanel {
                HStack(spacing: 10) {
                    TextField("mods", text: $m.modDir)
                        .foregroundColor(geText)
                        .padding(10)
                        .background(gePanel)
                        .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
                    Button(action: { m.rescanMods() }) {
                        Text("RESCAN")
                            .font(.system(size: 13, weight: .bold))
                            .foregroundColor(geText)
                            .padding(.horizontal, 16).padding(.vertical, 10)
                            .background(gePanel)
                            .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
                    }
                    .buttonStyle(GeButtonStyle())
                }
            }

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
        ScrollView {
            // Split into sections rather than one flat VStack -- see ControlsPage's
            // identical comment (mouseKeyboardSection) for why: this page has enough
            // GeSectionTitle+GePanel pairs to hit the same silent-EmptyView ceiling, and
            // three of these WERE silently missing on-screen (HD Textures, Crosshair,
            // Developer) before this split, confirmed on this machine the same way.
            VStack(alignment: .leading, spacing: 16) {
                displaySection
                imageQualitySection
                filteringSection
                timingSection
                hdTexturesSection
                crosshairSection
                developerSection
                Spacer()
            }
        }
    }

    #if os(macOS)
    // Desktop-only concepts -- tvOS is always fullscreen on the TV and iOS is always
    // fullscreen on the device, so neither platform's ge_launcher.cpp ImGui page showed
    // these either (they sit behind the same GE_PLATFORM_DESKTOP world this file's own
    // macOS branch belongs to).
    private var displaySection: some View {
        Group {
            GeSectionTitle(text: "Display")
            GePanel {
                VStack(alignment: .leading, spacing: 14) {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Resolution").foregroundColor(geDim).font(.system(size: 13))
                        TextField("1280x960", text: $m.resolution)
                            .foregroundColor(geText)
                            .padding(10)
                            .background(gePanel)
                            .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
                    }
                    Toggle(isOn: $m.fullscreen) {
                        Text("Fullscreen").foregroundColor(geText)
                    }
                }
            }
        }
    }
    #else
    private var displaySection: some View { EmptyView() }
    #endif

    private var imageQualitySection: some View {
        Group {
            GeSectionTitle(text: "Image Quality")
            GePanel {
                /* Supersample, MSAA, FXAA and anisotropic filtering all render for real
                 * under gfx_metal.mm's offscreen-postfx path -- tvOS and iOS are both
                 * always Metal, and Mac's OpenGL path has always supported all four
                 * natively, so these are live controls on every platform, not a
                 * renderer-specific gap to hide. Ranges match ge_launcher.cpp's own
                 * ImGui sliders (supersample 1-2, MSAA/aniso 0-8/0-16) rather than
                 * gfx_metal_init's wider 1-4 supersample clamp, since 2x is already the
                 * practical ceiling anyone would actually choose. */
                VStack(spacing: 14) {
                    GeStepper(label: "Supersampling", value: $m.supersample, range: 1...2, suffix: "x")
                    GeStepper(label: "MSAA", value: $m.msaa, range: 0...8, suffix: "x")
                    GeStepper(label: "Anisotropic filtering", value: $m.aniso, range: 0...16, suffix: "x")
                    GeStepper(label: "Field of view", value: $m.fov, range: 60...140, step: 5)
                }
                Toggle(isOn: $m.fxaaOn) {
                    Text("FXAA").foregroundColor(geText)
                }
                .padding(.top, 4)
                Toggle(isOn: $m.mipmaps) {
                    Text("Mipmapping").foregroundColor(geText)
                }
                Toggle(isOn: $m.widescreen) {
                    Text("Widescreen").foregroundColor(geText)
                }
            }
        }
    }

    private var filteringSection: some View {
        Group {
            GeSectionTitle(text: "Texture Filtering")
            GePanel {
                HStack(spacing: 10) {
                    ForEach(Array(["Nearest", "Bilinear", "Three-point"].enumerated()), id: \.offset) { i, label in
                        Button(action: { m.filtering = i }) {
                            Text(label)
                                .font(.system(size: 13, weight: m.filtering == i ? .bold : .regular))
                                .foregroundColor(m.filtering == i ? geGoldHi : geText)
                                .frame(maxWidth: .infinity)
                                .padding(.vertical, 10)
                                .background(m.filtering == i ? geGold.opacity(0.22) : gePanel)
                                .overlay(RoundedRectangle(cornerRadius: 3).stroke(geLine, lineWidth: 1))
                        }
                        .buttonStyle(GeButtonStyle())
                    }
                }
                Text("Three-point is what the N64's own RDP did: soft rather than blurry.")
                    .foregroundColor(geDim).font(.system(size: 12)).padding(.top, 4)
            }
        }
    }

    private var timingSection: some View {
        Group {
            GeSectionTitle(text: "Timing")
            GePanel {
                Toggle(isOn: $m.uncapped) {
                    Text("Uncapped (high refresh)").foregroundColor(geText)
                }
                if !m.uncapped {
                    GeStepper(label: "Frame rate cap", value: $m.framerate, range: 30...60, step: 30, suffix: " fps")
                        .padding(.top, 8)
                }
            }
        }
    }

    private var hdTexturesSection: some View {
        Group {
            /* Unlike the image-quality controls above, HD textures go through
             * gfx_pc.c's backend-agnostic ge_texpack_try_override() ->
             * GfxRenderingAPI.upload_texture, which gfx_metal.mm implements fully --
             * this is real on tvOS/iOS today, not a renderer gap, so it belongs on
             * this page regardless of backend. */
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
                        Toggle(isOn: $m.parallax) {
                            Text("Parallax from pack height maps").foregroundColor(geText)
                        }
                    }
                }
            }
        }
    }

    private var crosshairSection: some View {
        Group {
            GeSectionTitle(text: "Crosshair")
            GePanel {
                VStack(spacing: 14) {
                    GeStepper(label: "Reticle size", value: $m.crosshairScalePct, range: 25...200, suffix: "%")
                    HStack(spacing: 12) {
                        RoundedRectangle(cornerRadius: 4)
                            .fill(Color(red: Double(m.crosshairRPct) / 100.0,
                                        green: Double(m.crosshairGPct) / 100.0,
                                        blue: Double(m.crosshairBPct) / 100.0))
                            .frame(width: 28, height: 28)
                            .overlay(RoundedRectangle(cornerRadius: 4).stroke(geLine, lineWidth: 1))
                        Text("Color").foregroundColor(geDim).font(.system(size: 13))
                    }
                    GeStepper(label: "Red", value: $m.crosshairRPct, range: 0...100, suffix: "%")
                    GeStepper(label: "Green", value: $m.crosshairGPct, range: 0...100, suffix: "%")
                    GeStepper(label: "Blue", value: $m.crosshairBPct, range: 0...100, suffix: "%")
                }
            }
        }
    }

    private var developerSection: some View {
        Group {
            GeSectionTitle(text: "Developer")
            GePanel {
                Toggle(isOn: $m.devOverlay) {
                    Text("Show developer overlay in game").foregroundColor(geText)
                }
            }
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
    case mission, ruleset, horde, controls, mods, cheats, video, profile
    var title: String {
        switch self {
        case .mission: return "Mission"
        case .ruleset: return "Ruleset"
        case .horde: return "Horde"
        case .controls: return "Controls"
        case .mods: return "Mods"
        case .cheats: return "Cheats"
        case .video: return "Video"
        case .profile: return "Profile"
        }
    }
}

#if os(tvOS)
/* tvOS-only: unlike iOS (gePortVirtualControllerInit/ge_virtual_controller.mm falls back to
 * a real on-screen GCVirtualController when nothing physical is paired) and Mac (mouse and
 * keyboard always work), tvOS has NO fallback input path at all -- Siri Remote can navigate
 * this launcher's own focus-driven UI just fine, but the actual gameplay needs real analog
 * sticks port_input.c's geControllerIsReal() gate requires, which the remote cannot provide.
 * A first-time player who has never paired a controller would reach a game that silently
 * does nothing the moment they press Start Mission, with no signal anywhere about why. This
 * banner is that signal: live, not just a one-time check, since a player may plausibly open
 * the launcher, walk over to pair a controller, and come back without relaunching. */
private struct ControllerStatusBanner: View {
    @State private var hasController = !GCController.controllers().isEmpty

    var body: some View {
        Group {
            if !hasController {
                HStack(spacing: 10) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(geGold)
                    Text("No game controller paired. The Siri Remote can browse this menu, but gameplay needs a real controller -- pair one from tvOS Settings, then come back here.")
                        .foregroundColor(geText)
                        .font(.system(size: 14))
                }
                .padding(.horizontal, 24).padding(.vertical, 10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(geGold.opacity(0.15))
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            hasController = !GCController.controllers().isEmpty
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            hasController = !GCController.controllers().isEmpty
        }
    }
}
#endif

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

                #if os(tvOS)
                ControllerStatusBanner()
                #endif

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
                        case .controls: ControlsPage(m: m)
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

// MARK: - Bridge to ge_tvos_main.c / ge_mac_main.c

#if os(macOS)
// macOS has no focus-engine equivalent of tvOS's UIFocusSystem seeding problem (mouse/
// keyboard/trackpad hit-testing and tab-order focus are both live the moment a window is
// key, with no separate "nothing is focused yet" state to seed) and no scene delegate to
// hand a window to -- SDL itself never runs NSApplicationMain-equivalent setup either, so
// this is the first code in this whole port that has to bring up AppKit by hand. Conforms
// to NSWindowDelegate to detect the user closing the window via the red button, which is a
// real possible outcome on desktop unlike on tvOS/iOS: gePortLauncherRun()'s own documented
// contract (ge_mac_main.c's header comment) is "0 to carry on into the game, non-zero if
// the user closed the window without playing" -- this preserves that contract exactly, so
// ge_mac_main.c needs no changes beyond which function it calls.
private final class GeLauncherBridgeRunner: NSObject, NSWindowDelegate {
    static let shared = GeLauncherBridgeRunner()
    private var window: NSWindow?
    private var finished = false
    private var closedWithoutStarting = false

    func windowWillClose(_ notification: Notification) {
        closedWithoutStarting = true
    }

    func run() -> Int32 {
        // See the iOS/tvOS branch below for why GETV_LAUNCHER_AUTOPLAY short-circuits
        // identically here: automated boots need no window to tap, and model_load()+
        // model_store() alone already reproduce what a real "Start Mission" click leaves
        // behind. Unlike the old ImGui gePortLauncherRun(), this path is unconditional --
        // there is no argv/--launcher gate here (see this file's own top-of-class note on
        // that behaviour change) -- so this is the ONLY early-exit before a window shows.
        if ProcessInfo.processInfo.environment["GETV_LAUNCHER_AUTOPLAY"] == "1" {
            geBridgeLoad()
            geBridgeSave()
            return 0
        }

        // A bare command-line-style main() (port/mac/ge_mac_main.c) never triggers AppKit's
        // usual application-lifecycle bootstrap the way an Xcode @main App or a nib-based
        // NSApplicationMain() would -- NSApp.shared lazily creates the shared instance, but
        // it starts as a background/accessory-style app with no Dock icon and cannot become
        // key/frontmost until its activation policy says so.
        //
        // NSApplication.shared, not the NSApp global, deliberately: NSApp is populated as a
        // SIDE EFFECT of NSApplication.shared (or the ObjC +sharedApplication) having been
        // called at least once -- it is not itself what performs that lazy setup. A normal
        // .app bootstrapped via NSApplicationMain()/the SwiftUI App protocol always calls it
        // for you before your own code runs, so NSApp is never seen unpopulated there. This
        // bare main() (ge_mac_main.c) has no such caller, so referencing the NSApp global
        // FIRST crashed with "Unexpectedly found nil while implicitly unwrapping an
        // Optional value" (confirmed on this exact Mac via lldb). Calling
        // NSApplication.shared here performs that lazy setup itself; NSApp is safe to use
        // for the rest of this function afterward, but app is used throughout instead so
        // nothing here depends on that ordering again.
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        app.activate(ignoringOtherApps: true)

        finished = false
        closedWithoutStarting = false
        let view = GeLauncherView(onStart: { [weak self] in self?.finished = true })

        // Centered, clamped to the visible screen rather than a fixed design size -- the
        // seven-page/two-column layout wants real width, but a window requested larger
        // than the display just gets clipped off-screen rather than resized to fit.
        let screenFrame = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1200, height: 800)
        let w = min(1180, screenFrame.width - 60)
        let h = min(760, screenFrame.height - 60)
        let newWindow = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: w, height: h),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        newWindow.title = "GoldenEye 007"
        newWindow.isReleasedWhenClosed = false
        // NSHostingController sizes itself to its SwiftUI content's own fitting size, and
        // assigning it as contentViewController resizes the window to match THAT -- unlike
        // UIHostingController on iOS/tvOS, which just fills whatever UIWindow bounds it's
        // given. Without the explicit .frame below, GeLauncherView's own ideal size (nothing
        // here declares one) collapses the window down to a few hundred points on the real
        // Mac this was tested on. minWidth/minHeight matched to (w,h) below makes the
        // fitting-size calculation resolve to the size this function actually asked for;
        // setContentSize afterward is belt-and-suspenders against any rounding in that
        // calculation, not a substitute for it.
        newWindow.contentViewController = NSHostingController(
            rootView: view.frame(minWidth: w, idealWidth: w, minHeight: h, idealHeight: h))
        newWindow.setContentSize(NSSize(width: w, height: h))
        newWindow.center()
        newWindow.delegate = self
        newWindow.makeKeyAndOrderFront(nil)
        window = newWindow

        // Manual Cocoa event pump, playing the same role as the UIKit branch's
        // CFRunLoopRunInMode loop below: this function is called synchronously from
        // SDL_main() (via ge_mac_main.c's main(), before SDL itself initializes), so
        // nothing else is pumping NSApplication's event queue yet -- there is no
        // NSApp.run() anywhere in this process. NSApp.run() itself is deliberately NOT
        // used here: it does not return until the whole application terminates, not just
        // until this one window closes, which is the wrong lifetime for a launcher that
        // has to hand control back to this same function's caller.
        while !finished && !closedWithoutStarting {
            if let event = app.nextEvent(matching: .any, until: Date().addingTimeInterval(0.1),
                                          inMode: .default, dequeue: true) {
                app.sendEvent(event)
            }
        }

        window?.delegate = nil
        if !closedWithoutStarting { window?.close() }
        window = nil
        return closedWithoutStarting ? 1 : 0
    }
}
#else
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
        //
        // The single seed pass above can run before UIHostingController has actually
        // installed SwiftUI's focusable items -- its view controller lifecycle is lazy,
        // so assigning rootViewController does not guarantee the view (and therefore its
        // focus items) exist yet on the frame that call lands in. When that race is lost,
        // the one-shot seed finds nothing to focus and there is no retry, leaving the
        // window permanently focus-dead -- Siri Remote/controller input looks completely
        // ignored for the launcher's entire lifetime. Re-seed on the run loop's own early
        // spins (cheap and a no-op once something really is focused) until the focus
        // system reports an actual focused item, capped so this never runs forever.
        var focusSeedAttempts = 0
        while !finished {
            CFRunLoopRunInMode(.defaultMode, 0.05, true)
            if focusSeedAttempts < 20,
               UIFocusSystem.focusSystem(for: newWindow)?.focusedItem == nil {
                newWindow.setNeedsFocusUpdate()
                newWindow.updateFocusIfNeeded()
                focusSeedAttempts += 1
            }
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
#endif

@_cdecl("gePortNativeLauncherRun")
public func gePortNativeLauncherRun() -> Int32 {
    return GeLauncherBridgeRunner.shared.run()
}
#endif
