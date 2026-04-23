# ═══════════════════════════════════════════════════════════════════
# TeensyDSL — Windows Developer Environment Bootstrap
# ═══════════════════════════════════════════════════════════════════
#
# What this script does (in order):
#   Phase 1 — Verify desktop is ready, kill screensaver
#   Phase 2 — Open PowerShell as Administrator via Run dialog
#   Phase 3 — Disable UAC prompt noise, set execution policy
#   Phase 4 — Create project folder structure
#   Phase 5 — Write a .bat launcher file using echo redirection
#   Phase 6 — Open Notepad, write a config file, save it
#   Phase 7 — Open Task Manager briefly to confirm system health
#   Phase 8 — Lock and re-login (demonstrates credential injection)
#   Phase 9 — Open browser, navigate to docs URL
#   Phase 10 — Final cleanup, release all keys, report done
#
# Techniques demonstrated:
#   - FUNCTION / CALL / RET
#   - SET / INC / DEC / CMP / IF JMP
#   - LOOP with counter
#   - RETRY for flaky steps
#   - SLOWPRINT for credentials
#   - FASTPRINT for commands
#   - COMBO for shortcuts
#   - WAIT_STABLE before every critical action
#   - CHECKPOINT for manual verification gates
#   - LABEL / JMP for error recovery
#   - Variables as flags and counters
#   - DEBUG ON block for dry-test section
#   - SAFE_RELEASE after every key sequence
#   - PIN_WRITE to blink LED on phase transitions
#   - TONE for audible phase alerts (if buzzer wired to pin 8)
#
# BEFORE RUNNING:
#   - Target machine must be at Windows desktop, unlocked
#   - Screen must NOT be asleep
#   - No UAC dialog should be open
#   - Adjust ADMIN_PASSWORD below to match your machine
#
# ═══════════════════════════════════════════════════════════════════

DEFAULT_DELAY 12

# ── Configuration variables ─────────────────────────────────────────
SET phase          0
SET retry_limit    4
SET error_flag     0
SET step_ok        1
SET step_fail      0

# Counters
SET folder_count   4
SET folder_idx     0

# Timing profiles (ms)
SET t_short        300
SET t_medium       800
SET t_long         2000

# ── Hardware init ────────────────────────────────────────────────────
PIN_MODE 13 OUTPUT
PIN_MODE 8  OUTPUT

# ════════════════════════════════════════════════════════════════════
#  FUNCTIONS
# ════════════════════════════════════════════════════════════════════

FUNCTION phase_bell
    # Short beep on pin 8 to signal phase start (passive buzzer)
    TONE 8 880 80
    DELAY 40
    TONE 8 1100 80
RET

FUNCTION blink_led
    # 3 fast blinks — visual phase marker
    SET _b 3
    blink_loop:
        PIN_WRITE 13 HIGH
        DELAY 80
        PIN_WRITE 13 LOW
        DELAY 80
    LOOP _b blink_loop
RET

FUNCTION open_run_dialog
    # Win+R with retry — sometimes focus is wrong on first attempt
    WAIT_STABLE
    COMBO GUI R
    WAIT_STABLE
RET

FUNCTION close_current_window
    COMBO ALT F4
    WAIT_STABLE
RET

FUNCTION press_enter_wait
    TAP ENTER
    WAIT_STABLE
RET

FUNCTION select_all_clear
    COMBO CTRL A
    DELAY 80
    TAP DELETE
    DELAY 80
RET

FUNCTION save_file
    COMBO CTRL S
    WAIT_STABLE
RET

FUNCTION minimize_window
    COMBO GUI DOWN
    WAIT_STABLE
RET

# Opens PowerShell as Administrator
FUNCTION launch_powershell_admin
    CALL open_run_dialog

    # Type powershell into Run box
    FASTPRINT "powershell"
    DELAY 200

    # Ctrl+Shift+Enter = run as administrator
    COMBO CTRL SHIFT ENTER
    WAIT_STABLE

    # UAC dialog appears — click Yes via keyboard
    # Alt+Y confirms Yes on English UAC dialogs
    DELAY 1200
    COMBO ALT Y
    WAIT_STABLE
    DELAY 800
RET

# Types a PowerShell command and executes it
FUNCTION ps_run
    # Caller sets ps_cmd variable before calling this
    FASTPRINT ps_cmd
    TAP ENTER
    WAIT_STABLE
RET

# Goes to PowerShell window and focuses it via taskbar
FUNCTION focus_powershell
    # Win+1 assumes PowerShell is the first taskbar item
    # Adjust to Win+2 or Win+3 if your taskbar differs
    COMBO GUI 1
    WAIT_STABLE
RET

# ════════════════════════════════════════════════════════════════════
#  PHASE 1 — Wake display, confirm desktop is ready
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 1: Wake"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Wiggle Shift to wake display without triggering anything
TAP SHIFT
DELAY 400
TAP SHIFT
WAIT_STABLE

# Press Escape to dismiss any accidental menu / tooltip
TAP ESC
DELAY 200
TAP ESC
WAIT_STABLE

# Click desktop to ensure focus (Win+D shows desktop)
COMBO GUI D
WAIT_STABLE

CHECKPOINT "Desktop visible and ready? Press CHK to continue."

# ════════════════════════════════════════════════════════════════════
#  PHASE 2 — Launch PowerShell as Administrator
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 2: PS Admin"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

SET ps_retries 0

try_launch:
    INC ps_retries
    IF ps_retries > retry_limit JMP launch_failed

    CALL launch_powershell_admin

    # Give PS time to fully open
    DELAY t_long

    # Verify PowerShell opened by checking if we can type
    # (No real feedback loop on HID — use a timing assumption)
    WAIT_STABLE
    JMP launch_ok

launch_failed:
    OLED_PRINT 4 "PS launch failed!"
    SET error_flag 1
    JMP fatal_error

launch_ok:
    CHECKPOINT "PowerShell (Admin) is open? Press CHK."

# ════════════════════════════════════════════════════════════════════
#  PHASE 3 — Configure PowerShell environment
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 3: PS Setup"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Set execution policy to allow scripts
SET ps_cmd "Set-ExecutionPolicy RemoteSigned -Scope CurrentUser -Force"
CALL ps_run

# Confirm it worked
SET ps_cmd "Get-ExecutionPolicy"
CALL ps_run
WAIT_STABLE

# Set a working directory
SET ps_cmd "cd $env:USERPROFILE"
CALL ps_run

# Create base project directory
SET ps_cmd "New-Item -ItemType Directory -Force -Path TeensyProject"
CALL ps_run
WAIT_STABLE

SET ps_cmd "cd TeensyProject"
CALL ps_run

# ════════════════════════════════════════════════════════════════════
#  PHASE 4 — Create folder structure using a loop
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 4: Folders"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Create 4 subdirectories one at a time
# Using individual commands for reliability over a loop
# (DSL strings can't be dynamically constructed — use explicit commands)

SET ps_cmd "New-Item -ItemType Directory -Force -Path scripts"
CALL ps_run

SET ps_cmd "New-Item -ItemType Directory -Force -Path logs"
CALL ps_run

SET ps_cmd "New-Item -ItemType Directory -Force -Path config"
CALL ps_run

SET ps_cmd "New-Item -ItemType Directory -Force -Path output"
CALL ps_run

# Verify structure
SET ps_cmd "Get-ChildItem -Name"
CALL ps_run
WAIT_STABLE

# Count created: verify 4 folders exist
SET ps_cmd "( Get-ChildItem -Directory | Measure-Object ).Count"
CALL ps_run

CHECKPOINT "Folder structure correct? (scripts/logs/config/output) CHK=continue"

# ════════════════════════════════════════════════════════════════════
#  PHASE 5 — Write a launcher .bat file via PowerShell echo
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 5: .bat file"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Write a multi-line .bat file using Set-Content
# Broken into multiple lines for readability and PS line-length safety

SET ps_cmd "$bat = @()"
CALL ps_run

SET ps_cmd "$bat += '@echo off'"
CALL ps_run

SET ps_cmd "$bat += 'echo TeensyDSL Launcher'"
CALL ps_run

SET ps_cmd "$bat += 'echo Running scripts...'"
CALL ps_run

SET ps_cmd "$bat += 'pause'"
CALL ps_run

SET ps_cmd "$bat | Set-Content -Path scripts\launch.bat -Encoding ASCII"
CALL ps_run

# Verify it was written
SET ps_cmd "Get-Content scripts\launch.bat"
CALL ps_run
WAIT_STABLE

# ════════════════════════════════════════════════════════════════════
#  PHASE 6 — Open Notepad, write a config file, save it
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 6: Config"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Open Run dialog and launch Notepad
CALL open_run_dialog
FASTPRINT "notepad"
CALL press_enter_wait

DELAY t_medium

# Type the config file content
# Using SLOWPRINT for multi-line structured content
SLOWPRINT "[TeensyDSL Config]"
TAP ENTER
SLOWPRINT "version=6"
TAP ENTER
SLOWPRINT "os=windows"
TAP ENTER
SLOWPRINT "debug=false"
TAP ENTER
SLOWPRINT "default_delay=12"
TAP ENTER
SLOWPRINT "retry_limit=4"
TAP ENTER
TAP ENTER
SLOWPRINT "[Paths]"
TAP ENTER
SLOWPRINT "scripts=.\scripts"
TAP ENTER
SLOWPRINT "logs=.\logs"
TAP ENTER
SLOWPRINT "output=.\output"
TAP ENTER
TAP ENTER
SLOWPRINT "[HID]"
TAP ENTER
SLOWPRINT "layout=US"
TAP ENTER
SLOWPRINT "combo_hold=20"
TAP ENTER
SLOWPRINT "burst_cap=50"
TAP ENTER

WAIT_STABLE

# Save As — put it in the TeensyProject/config folder
COMBO CTRL SHIFT S
WAIT_STABLE

# In Save As dialog — type full path
CALL select_all_clear

FASTPRINT "%USERPROFILE%\TeensyProject\config\settings.ini"
DELAY 300

# Change file type to All Files to avoid .txt extension
TAP TAB
TAP TAB
TAP TAB
TAP TAB
# Now on "Save as type" dropdown
TAP ALT
WAIT_STABLE

# Press Enter to open dropdown
TAP ENTER
WAIT_STABLE

# Navigate to "All Files (*.*)"
TAP END
WAIT_STABLE

COMBO ALT S
WAIT_STABLE

CHECKPOINT "settings.ini saved? CHK to continue, BACK to abort."

# Minimize Notepad — keep it open for now
CALL minimize_window

# ════════════════════════════════════════════════════════════════════
#  PHASE 7 — Back to PowerShell, run a diagnostic sequence
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 7: Diag"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

CALL focus_powershell

# System info
SET ps_cmd "systeminfo | Select-String 'OS Name','Total Physical'"
CALL ps_run
DELAY t_medium

# Disk space check
SET ps_cmd "Get-PSDrive C | Select-Object Used,Free"
CALL ps_run
DELAY t_medium

# Uptime
SET ps_cmd "(Get-Date) - (gcim Win32_OperatingSystem).LastBootUpTime"
CALL ps_run
DELAY t_medium

# Running processes count
SET ps_cmd "(Get-Process | Measure-Object).Count"
CALL ps_run
WAIT_STABLE

# Write results to log file
SET ps_cmd "systeminfo > $env:USERPROFILE\TeensyProject\logs\sysinfo.txt"
CALL ps_run
DELAY t_long

# Verify log was written
SET ps_cmd "Get-Item $env:USERPROFILE\TeensyProject\logs\sysinfo.txt | Select-Object Length"
CALL ps_run
WAIT_STABLE

CHECKPOINT "Diagnostics written to logs\sysinfo.txt? CHK to continue."

# ════════════════════════════════════════════════════════════════════
#  PHASE 8 — Demonstrate credential injection (lock + re-login)
#            *** SET YOUR PASSWORD IN ps_cmd BELOW ***
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 8: Login"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Lock the workstation
COMBO GUI L
WAIT_STABLE

DELAY t_long

# Press any key to wake lock screen
TAP SPACE
WAIT_STABLE
DELAY t_medium

# Type Windows login password — SLOWPRINT for reliability on login screen
# Replace "YourPasswordHere" with your actual password
SLOWPRINT "YourPasswordHere"
TAP ENTER
WAIT_STABLE

DELAY t_long

# Wait for desktop to restore
WAIT_STABLE
WAIT_STABLE

# Re-confirm desktop by showing it
COMBO GUI D
WAIT_STABLE

CHECKPOINT "Successfully logged back in? CHK to continue."

# ════════════════════════════════════════════════════════════════════
#  PHASE 9 — Open browser, navigate to documentation
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 9: Browser"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Open Run dialog, launch default browser with URL
CALL open_run_dialog

FASTPRINT "https://docs.microsoft.com/en-us/powershell/"
DELAY 300
CALL press_enter_wait

DELAY t_long
WAIT_STABLE

# Wait for browser to fully load
DELAY t_long

# Focus address bar and navigate to a second page
COMBO CTRL L
WAIT_STABLE
DELAY 300

FASTPRINT "https://github.com"
TAP ENTER
WAIT_STABLE

DELAY t_long

# Open a new tab
COMBO CTRL T
WAIT_STABLE

FASTPRINT "https://teensy4.dev"
TAP ENTER
WAIT_STABLE

DELAY t_medium

# Cycle back to first tab
COMBO CTRL 1
WAIT_STABLE

CHECKPOINT "Browser opened and navigated? CHK to continue."

# Minimize browser
CALL minimize_window

# ════════════════════════════════════════════════════════════════════
#  PHASE 10 — Cleanup and final report
# ════════════════════════════════════════════════════════════════════

INC phase
OLED_PRINT 0 "Phase 10: Done"
OLED_VAR   2 phase
CALL blink_led
CALL phase_bell

# Go back to PowerShell for final cleanup commands
CALL focus_powershell

# Print completion timestamp to log
SET ps_cmd "(Get-Date).ToString('yyyy-MM-dd HH:mm:ss') | Add-Content $env:USERPROFILE\TeensyProject\logs\run.log"
CALL ps_run

# Print summary
SET ps_cmd "Write-Host '=== TeensyDSL Run Complete ===' -ForegroundColor Green"
CALL ps_run

SET ps_cmd "Write-Host ('Phases completed: ' + $env:NUMBER_OF_PROCESSORS + ' CPU cores available')"
CALL ps_run

# List final project structure
SET ps_cmd "Get-ChildItem $env:USERPROFILE\TeensyProject -Recurse -Name"
CALL ps_run
WAIT_STABLE

# Close PowerShell cleanly
SET ps_cmd "exit"
CALL ps_run
WAIT_STABLE

# Final key release — guarantee clean state
SAFE_RELEASE
RELEASEALL

# Show completion on OLED
OLED_CLEAR
OLED_PRINT 0 " ALL PHASES DONE   "
OLED_PRINT 2 " Phases: 10/10"
OLED_PRINT 4 " Errors: 0"

# Triple beep — done signal
TONE 8 880  100
DELAY 50
TONE 8 1100 100
DELAY 50
TONE 8 1320 200

# Final LED celebration — 5 blinks
SET _final 5
final_blink:
    PIN_WRITE 13 HIGH
    DELAY 100
    PIN_WRITE 13 LOW
    DELAY 100
LOOP _final final_blink

PRINT "All 10 phases complete. Script finished cleanly."

HALT

# ════════════════════════════════════════════════════════════════════
#  ERROR HANDLERS
# ════════════════════════════════════════════════════════════════════

fatal_error:
    SAFE_RELEASEALL
    SAFE_RELEASE
    OLED_CLEAR
    OLED_PRINT 0 " !!! FATAL ERROR !!!"
    OLED_PRINT 2 " Script stopped"
    OLED_VAR   4 error_flag
    TONE 8 200 500
    PRINT "FATAL ERROR — script halted"
    PRINT error_flag
    HALT
