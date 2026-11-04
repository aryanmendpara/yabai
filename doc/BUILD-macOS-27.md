# Building yabai for macOS 27.0 (Apple Silicon)

This guide walks through building, signing, installing and running this fork on
**macOS 27.0 (Apple Silicon / arm64)**. It is not limited to developers: if you
just want a working yabai on macOS 27, follow it top to bottom.

## What this fork changes

Four things differ from upstream `asmvik/yabai`:

1. **macOS 27 scripting-addition support** (`src/osax/arm64_payload.m`,
   `src/osax/common.h`, `src/osax/payload.m`).
   The scripting-addition (SA) locates private Dock symbols by scanning a
   window of Dock's `__TEXT` for a byte signature. macOS 27 moved several of
   them, so upstream's windows/signatures fail to resolve and space operations
   (`space --create`, `--destroy`, `--move`, …) break. This fork adds macOS 27
   windows and signatures:

   | Symbol | macOS 27 search window | Resolves? |
   |-:|:-|:-|
   | `dock.spaces` | `0x20000` | yes (`0x329be0` on 26B123) |
   | `fix_animation` | `0x160000` | yes |
   | `add_space` | `0x160000` | yes (`0x18a750`) |
   | `remove_space` | `0x120000` | yes (`0x12dbcc`) |
   | `dppm` (`DPDesktopPictureManager`) | — | no, class removed in 27.2 |
   | `move_space` | — | no, function removed in 27.2 (see item 2) |
   | `set_front_window` | — | no, hook is disabled |

2. **Space reordering on macOS 27.2** (`src/osax/payload.m`).
   Upstream moves spaces by calling a Dock-internal `moveSpace` function and
   then `-[DPDesktopPictureManager moveSpace:toDisplay:displayUUID:]` to keep
   wallpaper bookkeeping in sync. On macOS 27.2 the Dock no longer exposes
   `moveSpace` and the `DPDesktopPictureManager` class is gone (wallpaper is
   now handled by Dock's `WallpaperAgent`), so `space --move`, `space --swap`
   and `window --space` silently did nothing.

   This fork instead calls the same CoreGraphics API the 27.2 Dock itself uses
   from its Swift `DockCore` code when spaces are rearranged in Mission
   Control:

   ```c
   int CGSMoveManagedSpaceToDisplayIndex(int cid, uint64_t space_id,
                                         CFStringRef display_uuid, uint32_t index);
   ```

   The symbol is exported by CoreGraphics but not present in the arm64e SDK
   stubs, so it is resolved at runtime with `dlsym(RTLD_DEFAULT, …)`. The
   destination `index` is 0-based, counted within the destination display's
   space list **with the source space removed**, which makes
   `index(destination) + 1` equivalent to "place the source directly after the
   destination". Older macOS keeps the original `moveSpace` + `dppm` path
   because the CGS symbol is only used when it resolves.

   Consequences on macOS 27: `dppm`/`moveSpace` diagnostics are expected and
   harmless, the payload logs `using CGSMoveManagedSpaceToDisplayIndex`, and
   every space-order change goes through SkyLight/Dock, so Mission Control
   stays in sync. `setFrontWindow` still does not resolve and its only caller
   is disabled (`OSAX_ATTRIB_SET_WINDOW` is no longer part of
   `OSAX_ATTRIB_ALL`), so the SA still validates.

3. **Scripting-addition load fix** (`src/sa.m`).
   Upstream refuses to inject the SA on arm64 unless the boot-arg
   `-arm64e_preview_abi` is present. That boot-arg is only needed for the
   `thread_convert_thread_state()` injection path used before macOS 14.4, and
   macOS 26+ refuses to set it at all ([#2741][issue-2741]). This fork only
   requires it on macOS < 14.4, which makes `yabai --load-sa` work again on
   26/27.

4. **Main-binary macOS 27 recognition** (`src/workspace.m`).
   `src/workspace.h` only lists macOS versions up to Tahoe (26), and the main
   binary matches a major version **exactly**, so on macOS 27 every
   `workspace_is_macos_*()` predicate returned `false`. The binary then
   silently took legacy code paths: Mission Control was never observed, the
   space/window connection notifications (1327/1328 and 804) were not
   registered, `update_window_notifications()` was skipped, and the window
   sub-level query used the old message id `0x73C3` instead of `0x76E3`.
   macOS 27 has not diverged from Tahoe in any of these paths, so this fork
   aliases `majorVersion >= 27` to `26` before the version predicates are
   computed, restoring the Tahoe behaviour. If Apple changes one of these APIs
   in a later 27.x build, this is the first place to look.

Note: on macOS 27 the Dock no longer sends either the AX expose notifications or
the legacy SLS connection event (`1204`) that yabai used to detect Mission
Control. A 500 ms poll of the on-screen window list now watches for Mission
Control's chrome windows instead (`ExposeShieldWindow` at layer 19 and
`Spaces Bar` at layer 14, owned by the `WindowManager` process; older releases
used a Dock window at layer 18). Space commands no longer refuse to run while
Mission Control is open — the scripting-addition talks to SkyLight/Dock
directly, so they work with Mission Control on screen. Detection is still used
for the window/space reconciliation that runs when Mission Control closes —
without it, dragging a window to another space could leave a stale window↔space
mapping until yabai was restarted. On macOS 27 the
poll is authoritative in both directions: when no chrome window is on screen it
posts a (idempotent) exit, which also clears a mode that got stuck active. The
layer-18 `Dock` fallback and chrome windows with zero alpha or no area are
ignored on macOS 27 so the poll cannot report Mission Control while it is
closed.

## Prerequisites

- macOS 27.0 on Apple Silicon.
- Xcode Command Line Tools:
  ```
  xcode-select --install
  ```
- **System Integrity Protection partially disabled** — both *Filesystem
  Protections* and *Debugging Restrictions* must be off for the SA to inject
  into Dock.app. Boot into Recovery and run:
  ```
  csrutil enable --without fs --without debug
  ```
  (or configure the equivalent custom configuration). Verify with `csrutil status`.
- Optional but recommended: a self-signed code-signing certificate named
  **`yabai-cert`** (see [Signing](#3-sign-the-binary)). Using it keeps the
  binary's designated requirement stable, so the Accessibility grant survives
  rebuilds. Create it once via *Keychain Access → Certificate Assistant →
  Create a Certificate…* (name: `yabai-cert`, type: *Code Signing*).

## 1. Clone the repository

```
git clone https://github.com/VenusCrazy/yabai.git
cd yabai
```

## 2. Build

```
make install
```

This compiles both the CLI/daemon and the arm64e SA payload/loader, and writes
the result to `bin/yabai`. Confirm it:

```
./bin/yabai --version
```

## 3. Sign the binary

```
codesign --force --sign "yabai-cert" bin/yabai
```

If you did not create a certificate, sign ad-hoc instead — but note that the
Accessibility grant must then be re-added after every rebuild:

```
codesign --force --sign - bin/yabai
```

## 4. Install

Install to a stable location that Homebrew cannot overwrite, e.g.
`/usr/local/bin`:

```
sudo install -m 755 bin/yabai /usr/local/bin/yabai
sudo codesign --force --sign "yabai-cert" /usr/local/bin/yabai   # optional re-sign
```

If Homebrew's yabai is installed it will shadow this build in `$PATH`
(`/opt/homebrew/bin` precedes `/usr/local/bin`). Remove it:

```
brew uninstall yabai
```

## 5. Register the launchd service

```
/usr/local/bin/yabai --stop-service        # if an old service exists
/usr/local/bin/yabai --uninstall-service   # if an old plist exists
/usr/local/bin/yabai --start-service
```

The generated `~/Library/LaunchAgents/com.asmvik.yabai.plist` will point at
`/usr/local/bin/yabai`. Checking `launchctl print gui/$(id -u)/com.asmvik.yabai`
should show `state = running`.

## 6. Grant Accessibility

yabai aborts with `could not access accessibility features!` until it is
granted. Open *System Settings → Privacy & Security → Accessibility*, remove
any stale entry, click **+**, press **Cmd+Shift+G**, enter
`/usr/local/bin/yabai` and enable it. Then restart the service:

```
/usr/local/bin/yabai --restart-service
```

## 7. Scripting-addition

### 7.1 First-time install

```
sudo /usr/local/bin/yabai --load-sa
```

This installs the SA (`/Library/ScriptingAdditions/yabai.osax`, version 2.1.31)
and injects the payload into Dock.app. On success a socket appears at
`/tmp/yabai-sa_<user>.socket`.

### 7.2 Keep it loaded across logins and Dock restarts

On macOS 27, Dock does **not** automatically load the SA, so it has to be
re-injected after every login and whenever Dock restarts. The standard yabai
approach is a signal plus a passwordless sudo rule.

The rule pins the sha256 of the installed binary, so it needs refreshing after
every upgrade (a stale rule makes the service fail with `sudo: a password is
required`). The `yabai-sudoers` helper does both steps in one go — writing the
rule for the current binary and reloading the SA:

```
yabai-sudoers          # from a Homebrew install
sudo scripts/install-sudoers.sh   # from a source checkout
```

Re-run it after upgrading yabai. Pass `--no-load` to only write the rule.

Or add the rule by hand (validate before installing):

```
echo "$(id -un) ALL=(root) NOPASSWD: sha256:$(shasum -a 256 /usr/local/bin/yabai | cut -d ' ' -f 1) /usr/local/bin/yabai --load-sa" | sudo tee /tmp/yabai-sudoers
sudo visudo -cf /tmp/yabai-sudoers && sudo install -m 440 -o root -g wheel /tmp/yabai-sudoers /private/etc/sudoers.d/yabai
```

Add these lines near the top of `~/.config/yabai/yabairc` (before any window
rules are configured):

```
yabai -m signal --add event=dock_did_restart action="sudo /usr/local/bin/yabai --load-sa"
sudo /usr/local/bin/yabai --load-sa
```

The signal re-injects the SA whenever Dock restarts, and the direct call
covers login (the service sources `yabairc` at startup).

## 8. Verify

```
yabai -m query --spaces          # list spaces
yabai -m space --create          # exercises the add_space SA hook
yabai -m space <n> --destroy     # exercises the remove_space SA hook
yabai -m space --move next       # exercises CGSMoveManagedSpaceToDisplayIndex
yabai -m space --swap <n>        # same code path as --move
yabai -m window <id> --space <n> # moves a window to another space
```

If `--create`/`--destroy` and `--move`/`--swap` succeed, the macOS 27 hooks
are resolving correctly. The payload logs each resolved hook at injection
time:

```
sudo log show --last 2m --info --predicate 'eventMessage CONTAINS "yabai-sa"'
```

Expected lines on macOS 27:

```
[yabai-sa] using CGSMoveManagedSpaceToDisplayIndex
[yabai-sa] dock.spaces found at address …
[yabai-sa] could not locate pointer to dppm! using CGSMoveManagedSpaceToDisplayIndex instead
[yabai-sa] addSpace found at address …
[yabai-sa] removeSpace found at address …
```

`could not locate pointer to dppm!`, `failed to get pointer to moveSpace
function!` and the missing `setFrontWindow` hook are expected on 27.x — those
Dock internals no longer exist and the CGS path replaces them (see
[What this fork changes](#what-this-fork-changes)). If you see `could not
resolve CGSMoveManagedSpaceToDisplayIndex` instead, space reordering will not
work on this OS build.

## 9. Rebuilding after changes

`scripts/deploy.sh` automates build → sign → install → reload:

```
scripts/deploy.sh                 # uses /usr/local/bin and yabai-cert
scripts/deploy.sh ~/.local/bin    # custom install directory
```

It falls back to ad-hoc signing (with a warning) when `yabai-cert` is missing.

## Troubleshooting

| Symptom | Cause / fix |
|:-|:-|
| `missing required nvram boot-arg '-arm64e_preview_abi'` | You are running an upstream or older build. Use this fork's binary (the check is scoped to macOS < 14.4 here). |
| `could not access accessibility features! abort..` | Grant Accessibility to the exact installed path (`/usr/local/bin/yabai`) and restart the service. |
| `cannot create/destroy space due to an error with the scripting-addition` | The SA is not injected. Run `sudo /usr/local/bin/yabai --load-sa` and check `/tmp/yabai-sa_<user>.socket`. |
| `space --move` / `--swap` / `window --space` returns success but nothing moves | The injected SA is old. `/usr/local/bin/yabai --uninstall-sa && sudo /usr/local/bin/yabai --load-sa`, then confirm the payload logs `using CGSMoveManagedSpaceToDisplayIndex`. |
| SA stops working after Dock restarts | Ensure the `dock_did_restart` signal and the sudoers rule from §7.2 are in place. |
| Window stuck at the screen edge after dragging it between spaces in Mission Control | Fixed in `v7.1.28-macos27.2`: Mission Control is detected by polling its chrome windows, and window/space membership is reconciled when it closes. |
| `cannot … because mission-control is active.` while Mission Control is open | Fixed in `v7.1.28-macos27.5`: space commands run while Mission Control is open. Upgrade, reinstall (`make install`), run `yabai-sudoers`, and `yabai --restart-service`. |
| `cannot … because mission-control is active.` while Mission Control is closed | Fixed in `v7.1.28-macos27.4`: the poll now clears a stuck Mission Control mode within 500 ms instead of only reacting to entering it. Upgrade, reinstall (`make install`), run `yabai-sudoers`, and `yabai --restart-service`. |
| `yabai: service file … already installed!` | Run `yabai --uninstall-service` before `--install-service`, or simply use `--start-service`. |
| `sudo: a password is required` in `/tmp/yabai_<user>.err.log` | The passwordless sudoers rule for `--load-sa` is missing or its path is wrong. |

Logs:

- service: `/tmp/yabai_<user>.out.log` and `/tmp/yabai_<user>.err.log`
- scripting-addition: `sudo log show --last 2m --info --predicate 'eventMessage CONTAINS "yabai-sa"'`

[issue-2741]: https://github.com/asmvik/yabai/issues/2741
