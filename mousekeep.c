/* Windows x64; no C runtime, heap allocation, polling, files, or network. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

static HANDLE stop_event;
static WCHAR mutex_name[192], stop_name[192];
static WCHAR args[4][192];
static WCHAR exe_path[32768], child_command[33024];

/* Compilers can emit memset for zero-initialized Win32 structures. */
void *memset(void *destination, int value, SIZE_T count)
{
    volatile BYTE *out = (volatile BYTE *)destination;
    while (count--) *out++ = (BYTE)value;
    return destination;
}

static void print(const char *text, BOOL error)
{
    DWORD length = 0, written;
    while (text[length]) ++length;
    WriteFile(GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE),
              text, length, &written, NULL);
}

static int fail(const char *text)
{
    print(text, TRUE);
    return 1;
}

static WCHAR *append(WCHAR *out, const WCHAR *text)
{
    while (*text) *out++ = *text++;
    *out = 0;
    return out;
}

static WCHAR *number(WCHAR *out, DWORD value)
{
    WCHAR digits[10];
    unsigned count = 0;
    do { digits[count++] = L'0' + value % 10; value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
    return out;
}

static BOOL equal(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* Only command tokens are parsed here. The executable path is skipped using
   Windows' special argv[0] quoting rules; no CRT or shell32 is needed. */
static int arguments(void)
{
    const WCHAR *p = GetCommandLineW();
    BOOL quoted = FALSE;
    int count = 0;
    while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
        if (*p == L'"') quoted = !quoted;
        ++p;
    }
    if (quoted) return -1;
    for (;;) {
        unsigned length = 0;
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) return count;
        if (count == 4) return -1;
        quoted = FALSE;
        while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'"') quoted = !quoted;
            else {
                if (length == 191) return -1;
                args[count][length++] = *p;
            }
            ++p;
        }
        if (quoted) return -1;
        args[count++][length] = 0;
    }
}

static BOOL seconds(const WCHAR *text, DWORD *value)
{
    DWORD n = 0;
    if (!*text) return FALSE;
    while (*text) {
        if (*text < L'0' || *text > L'9') return FALSE;
        /* Bound each step before multiplication, including very long input. */
        if (n > 8640) return FALSE;
        n = n * 10 + *text++ - L'0';
        if (n > 86400) return FALSE;
    }
    if (!n) return FALSE;
    *value = n;
    return TRUE;
}

/* Scope controls to this Windows user and logon session, even across copies
   of the executable. Default Windows object permissions protect the handles. */
static BOOL names(void)
{
    HANDLE token;
    union { TOKEN_USER user; BYTE bytes[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } info;
    DWORD needed, i, size;
    BYTE *sid;
    WCHAR *out;
    const WCHAR hex[] = L"0123456789abcdef";
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    if (!GetTokenInformation(token, TokenUser, &info, sizeof(info), &needed)) {
        CloseHandle(token);
        return FALSE;
    }
    CloseHandle(token);
    sid = (BYTE *)info.user.User.Sid;
    size = GetLengthSid(sid);
    if (size > SECURITY_MAX_SID_SIZE) return FALSE;
    out = append(mutex_name, L"Local\\MouseKeep.v1.");
    for (i = 0; i < size; ++i) {
        *out++ = hex[sid[i] >> 4];
        *out++ = hex[sid[i] & 15];
    }
    *out = 0;
    append(append(stop_name, mutex_name), L".stop");
    append(out, L".mutex");
    return TRUE;
}

static BOOL WINAPI control(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}

static BOOL dragging(void)
{
    return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
             GetAsyncKeyState(VK_MBUTTON) | GetAsyncKeyState(VK_XBUTTON1) |
             GetAsyncKeyState(VK_XBUTTON2)) & 0x8000) != 0;
}

static int run(DWORD interval, const WCHAR *ready_name)
{
    HANDLE mutex, ready = NULL;
    DWORD result;
    int code = 0;
    BOOL handler = FALSE, awake = FALSE;
    INPUT movement = {0};
    LONG direction = 1;
    mutex = CreateMutexW(NULL, FALSE, mutex_name);
    if (!mutex) return fail("Cannot create instance guard.\n");
    result = WaitForSingleObject(mutex, 0);
    if (result == WAIT_TIMEOUT) {
        CloseHandle(mutex);
        print("Already running.\n", FALSE);
        return 3;
    }
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return fail("Cannot acquire instance guard.\n");
    }
    stop_event = CreateEventW(NULL, TRUE, FALSE, stop_name);
    if (!stop_event) { code = fail("Cannot create stop event.\n"); goto done; }
    if (!ResetEvent(stop_event)) { code = fail("Cannot reset stop event.\n"); goto done; }
    if (!SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)) {
        code = fail("Cannot request sleep prevention.\n"); goto done;
    }
    awake = TRUE;
    if (ready_name) {
        ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
        if (!ready || !SetEvent(ready)) {
            code = fail("Cannot confirm startup.\n"); goto done;
        }
        CloseHandle(ready);
        ready = NULL;
    } else {
        handler = SetConsoleCtrlHandler(control, TRUE);
        if (!handler) { code = fail("Cannot install Ctrl+C handler.\n"); goto done; }
        print("Running. Ctrl+C to stop.\n", FALSE);
    }
    movement.type = INPUT_MOUSE;
    movement.mi.dwFlags = MOUSEEVENTF_MOVE;
    movement.mi.dwExtraInfo = 0x4D4B7631; /* Identifies our input during verification. */
    for (;;) {
        result = WaitForSingleObject(stop_event, interval * 1000);
        if (result == WAIT_OBJECT_0) break;
        if (result != WAIT_TIMEOUT) { code = fail("Wait failed.\n"); break; }
        if (!dragging()) {
            movement.mi.dx = direction;
            /* Alternate directions to avoid continual drift. Injection can be
               blocked on secure/elevated desktops; the power request remains. */
            SendInput(1, &movement, sizeof(movement));
            direction = -direction;
        }
    }
done:
    if (ready) CloseHandle(ready);
    if (awake) SetThreadExecutionState(ES_CONTINUOUS);
    if (handler) SetConsoleCtrlHandler(control, FALSE);
    /* Keep stop_event valid until ExitProcess: an in-flight control callback
       may still refer to it. Windows closes it at process exit. */
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}

static int start(DWORD interval)
{
    WCHAR ready_name[64], *out;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process;
    HANDLE ready, waits[2];
    DWORD result, code = 1, length;
    int exit_code;
    out = append(ready_name, L"Local\\MouseKeep.ready.");
    number(out, GetCurrentProcessId());
    ready = CreateEventW(NULL, TRUE, FALSE, ready_name);
    if (!ready) return fail("Cannot create startup event.\n");
    length = GetModuleFileNameW(NULL, exe_path, 32768);
    if (!length || length >= 32768) {
        CloseHandle(ready);
        return fail("Cannot locate executable.\n");
    }
    out = append(child_command, L"\"");
    out = append(out, exe_path);
    out = append(out, L"\" --worker ");
    out = number(out, interval);
    out = append(out, L" ");
    append(out, ready_name);
    startup.cb = sizeof(startup);
    if (!CreateProcessW(exe_path, child_command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        NULL, NULL, &startup, &process)) {
        CloseHandle(ready);
        return fail("Cannot start background process.\n");
    }
    CloseHandle(process.hThread);
    waits[0] = process.hProcess;
    waits[1] = ready;
    result = WaitForMultipleObjects(2, waits, FALSE, 10000);
    if (result == WAIT_OBJECT_0 + 1) {
        print("Started.\n", FALSE);
        exit_code = 0;
    } else if (result == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
        if (code == 3) { print("Already running.\n", FALSE); exit_code = 0; }
        else exit_code = fail("Background startup failed.\n");
    } else {
        /* Only terminate the exact child we created if startup never completed. */
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        exit_code = fail("Background startup timed out.\n");
    }
    CloseHandle(process.hProcess);
    CloseHandle(ready);
    return exit_code;
}

static int inspect(BOOL stop)
{
    HANDLE mutex, event;
    DWORD result, error;
    mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutex_name);
    if (!mutex) {
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) return fail("Cannot access instance guard.\n");
        print("Stopped.\n", FALSE);
        return stop ? 0 : 3;
    }
    result = WaitForSingleObject(mutex, 0);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        print("Stopped.\n", FALSE);
        return stop ? 0 : 3;
    }
    if (result != WAIT_TIMEOUT) { CloseHandle(mutex); return fail("Cannot query instance.\n"); }
    if (!stop) { CloseHandle(mutex); print("Running.\n", FALSE); return 0; }
    /* A foreground instance may still be initializing its stop event. */
    event = OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_name);
    if (!event) { CloseHandle(mutex); return fail("Cannot stop instance; retry shortly.\n"); }
    if (!SetEvent(event)) {
        CloseHandle(event); CloseHandle(mutex); return fail("Cannot signal stop.\n");
    }
    CloseHandle(event);
    result = WaitForSingleObject(mutex, 5000);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        ReleaseMutex(mutex);
        print("Stopped.\n", FALSE);
    } else { CloseHandle(mutex); return fail("Stop timed out.\n"); }
    CloseHandle(mutex);
    return 0;
}

static int help(BOOL error)
{
    print("mousekeep [seconds]         Start in background (default: 60).\n"
          "mousekeep start [seconds]   Start in background.\n"
          "mousekeep run [seconds]     Run until Ctrl+C.\n"
          "mousekeep status            Show running state.\n"
          "mousekeep stop              Stop.\n"
          "Interval: 1-86400 whole seconds.\n", error);
    return error ? 2 : 0;
}

static int main_command(void)
{
    int count = arguments();
    DWORD interval = 60;
    BOOL foreground = FALSE;
    const WCHAR *ready = NULL;
    if (count < 0) return help(TRUE);
    if (count == 1 && (equal(args[0], L"--help") || equal(args[0], L"-h"))) return help(FALSE);
    if (count == 1 && (equal(args[0], L"stop") || equal(args[0], L"status"))) {
        if (!names()) return fail("Cannot identify Windows user.\n");
        return inspect(equal(args[0], L"stop"));
    }
    if (count && (equal(args[0], L"run") || equal(args[0], L"start"))) {
        if (count > 2 || (count == 2 && !seconds(args[1], &interval))) return help(TRUE);
        foreground = equal(args[0], L"run");
    } else if (count && equal(args[0], L"--worker")) {
        if (count != 3 || !seconds(args[1], &interval)) return help(TRUE);
        foreground = TRUE;
        ready = args[2];
    } else if (count && (count != 1 || !seconds(args[0], &interval))) return help(TRUE);
    if (!names()) return fail("Cannot identify Windows user.\n");
    return foreground ? run(interval, ready) : start(interval);
}

/* Custom entry point deliberately avoids linking the C runtime. */
void mainCRTStartup(void)
{
    ExitProcess(main_command());
}
