extern struct event_loop g_event_loop;
extern enum mission_control_mode g_mission_control_mode;
extern volatile uint64_t __last_cmd_tab_time;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
static CONNECTION_CALLBACK(connection_handler)
{
    if (type == 1204) {
        event_loop_post(&g_event_loop, MISSION_CONTROL_ENTER, NULL, 0);
    } else if (type == 1327) {
        uint64_t sid; memcpy(&sid, data, sizeof(uint64_t));
        event_loop_post(&g_event_loop, SLS_SPACE_CREATED, (void *) (intptr_t) sid , 0);
    } else if (type == 1328) {
        uint64_t sid; memcpy(&sid, data, sizeof(uint64_t));
        event_loop_post(&g_event_loop, SLS_SPACE_DESTROYED, (void *) (intptr_t) sid, 0);
    } else if (type == 808) {
        uint32_t wid; memcpy(&wid, data, sizeof(uint32_t));
        event_loop_post(&g_event_loop, SLS_WINDOW_ORDERED, (void *) (intptr_t) wid, 0);
    } else if (type == 804) {
        uint32_t wid; memcpy(&wid, data, sizeof(uint32_t));
        event_loop_post(&g_event_loop, SLS_WINDOW_DESTROYED, (void *) (intptr_t) wid, 0);
    } else if (type == 1202) {
        __atomic_store_n(&__last_cmd_tab_time, read_os_timer(), __ATOMIC_RELEASE);
    }
}
#pragma clang diagnostic pop

enum mission_control_mode
{
    MISSION_CONTROL_MODE_INACTIVE           = 0,
    MISSION_CONTROL_MODE_SHOW               = 1,
    MISSION_CONTROL_MODE_SHOW_ALL_WINDOWS   = 2,
    MISSION_CONTROL_MODE_SHOW_FRONT_WINDOWS = 3,
    MISSION_CONTROL_MODE_SHOW_DESKTOP       = 4
};

static const char *mission_control_mode_str[] = {
    [MISSION_CONTROL_MODE_INACTIVE]           = "inactive",
    [MISSION_CONTROL_MODE_SHOW]               = "show",
    [MISSION_CONTROL_MODE_SHOW_ALL_WINDOWS]   = "show-all-windows",
    [MISSION_CONTROL_MODE_SHOW_FRONT_WINDOWS] = "show-front-windows",
    [MISSION_CONTROL_MODE_SHOW_DESKTOP]       = "show-desktop"
};

static struct {
    AXUIElementRef ref;
    AXObserverRef observer_ref;
    bool is_observing;
} g_mission_control_observer;

static CFStringRef kAXExposeShowAllWindows   = CFSTR("AXExposeShowAllWindows");
static CFStringRef kAXExposeShowFrontWindows = CFSTR("AXExposeShowFrontWindows");
static CFStringRef kAXExposeShowDesktop      = CFSTR("AXExposeShowDesktop");
static CFStringRef kAXExposeExit             = CFSTR("AXExposeExit");

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
static OBSERVER_CALLBACK(mission_control_notification_handler)
{
    if (CFEqual(notification, kAXExposeShowAllWindows)) {
        event_loop_post(&g_event_loop, MISSION_CONTROL_SHOW_ALL_WINDOWS, NULL, 0);
    } else if (CFEqual(notification, kAXExposeShowFrontWindows)) {
        event_loop_post(&g_event_loop, MISSION_CONTROL_SHOW_FRONT_WINDOWS, NULL, 0);
    } else if (CFEqual(notification, kAXExposeShowDesktop)) {
        event_loop_post(&g_event_loop, MISSION_CONTROL_SHOW_DESKTOP, NULL, 0);
    } else if (CFEqual(notification, kAXExposeExit)) {
        event_loop_post(&g_event_loop, MISSION_CONTROL_EXIT, NULL, 0);
    }
}
#pragma clang diagnostic pop

void mission_control_observe(void)
{
    if (!g_mission_control_observer.is_observing) {
        uint32_t pid = workspace_get_dock_pid();
        g_mission_control_observer.ref = AXUIElementCreateApplication(pid);

        if (pid && g_mission_control_observer.ref) {
            if (AXObserverCreate(pid, mission_control_notification_handler, &g_mission_control_observer.observer_ref) == kAXErrorSuccess) {
                AXObserverAddNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeShowAllWindows, NULL);
                AXObserverAddNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeShowFrontWindows, NULL);
                AXObserverAddNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeShowDesktop, NULL);
                AXObserverAddNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeExit, NULL);

                g_mission_control_observer.is_observing = true;
                CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(g_mission_control_observer.observer_ref), kCFRunLoopDefaultMode);
            }
        }
    }
}

void mission_control_unobserve(void)
{
    if (g_mission_control_observer.is_observing) {
        AXObserverRemoveNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeShowAllWindows);
        AXObserverRemoveNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeShowFrontWindows);
        AXObserverRemoveNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeShowDesktop);
        AXObserverRemoveNotification(g_mission_control_observer.observer_ref, g_mission_control_observer.ref, kAXExposeExit);

        g_mission_control_observer.is_observing = false;
        CFRunLoopSourceInvalidate(AXObserverGetRunLoopSource(g_mission_control_observer.observer_ref));
        CFRelease(g_mission_control_observer.observer_ref);
        CFRelease(g_mission_control_observer.ref);
    }
}

static inline bool mission_control_is_active(void)
{
    return g_mission_control_mode != MISSION_CONTROL_MODE_INACTIVE;
}

//
// NOTE: The Dock no longer exposes Mission Control through AX notifications (kAXExpose*)
// or the old SLS connection event on macOS 27, so fall back to detecting the chrome
// windows that Mission Control puts on screen. Older systems render them from the Dock
// at layer 18, macOS 27 renders them from the WindowManager process at layer 19
// (ExposeShieldWindow) and layer 14 (Spaces Bar).
//

static bool g_mission_control_poll_authoritative = false;
static bool g_mission_control_dock_chrome = true;

static bool mission_control_is_visible(void)
{
    CFArrayRef window_list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!window_list) return false;

    bool found = false;
    int window_count = CFArrayGetCount(window_list);

    for (int i = 0; i < window_count; ++i) {
        CFDictionaryRef dictionary = CFArrayGetValueAtIndex(window_list, i);

        CFStringRef owner = CFDictionaryGetValue(dictionary, kCGWindowOwnerName);
        if (!owner) continue;

        CFNumberRef layer_ref = CFDictionaryGetValue(dictionary, kCGWindowLayer);
        if (!layer_ref) continue;

        uint64_t layer = 0;
        CFNumberGetValue(layer_ref, CFNumberGetType(layer_ref), &layer);

        //
        // NOTE: ignore windows that are not actually rendered. Mission Control chrome
        // that lingers in the window list with zero alpha or no area used to be treated
        // as visible and left mission control mode stuck on.
        //

        CFNumberRef alpha_ref = CFDictionaryGetValue(dictionary, kCGWindowAlpha);
        double alpha = 1.0;
        if (alpha_ref) CFNumberGetValue(alpha_ref, kCFNumberDoubleType, &alpha);
        if (alpha <= 0.01) continue;

        CFDictionaryRef bounds_dict = CFDictionaryGetValue(dictionary, kCGWindowBounds);
        CGRect bounds = CGRectZero;
        if (bounds_dict) CGRectMakeWithDictionaryRepresentation(bounds_dict, &bounds);
        if (bounds.size.width <= 1 || bounds.size.height <= 1) continue;

        if (CFEqual(owner, CFSTR("WindowManager")) && (layer == 19 || layer == 14)) {
            found = true;
            break;
        }

        if (g_mission_control_dock_chrome && CFEqual(owner, CFSTR("Dock")) && layer == 18) {
            found = true;
            break;
        }
    }

    CFRelease(window_list);
    return found;
}

static void mission_control_poll(void *context)
{
    (void)context;

    bool visible = mission_control_is_visible();
    bool active = mission_control_is_active();

    //
    // NOTE: compare the actual state against the detected state instead of tracking
    // transitions. On macOS 27 the poll is the only source of truth, so a mode that
    // got stuck active (or a spurious enter) is corrected on the next tick.
    //

    if (!active && visible) {
        debug("MC-POLL: enter\n");
        event_loop_post(&g_event_loop, MISSION_CONTROL_ENTER, NULL, 0);
    } else if (active && !visible && (g_mission_control_poll_authoritative || g_mission_control_mode == MISSION_CONTROL_MODE_SHOW)) {
        debug("MC-POLL: exit\n");
        event_loop_post(&g_event_loop, MISSION_CONTROL_EXIT, NULL, 0);
    }
}

void mission_control_poll_begin(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSOperatingSystemVersion os_version = [[NSProcessInfo processInfo] operatingSystemVersion];
        g_mission_control_poll_authoritative = os_version.majorVersion >= 27;
        g_mission_control_dock_chrome = os_version.majorVersion < 27;

        dispatch_queue_t queue = dispatch_queue_create("yabai.mission_control", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 500ull * NSEC_PER_MSEC, 50ull * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{ mission_control_poll(NULL); });
        dispatch_resume(timer);
    });
}
