# Testing Checklist - FH6 Radio Mod

## Before Starting
1. Ensure the game is fully closed
2. Clear old Proton log: `rm /home/devu/steam-2483190.log`
3. Ensure the new DLL is copied: `~/.local/share/Steam/steamapps/common/ForzaHorizon6/version.dll`

## Test Steps

### Step 1: Launch with Debug Logging
```bash
cd ~/.local/share/Steam/steamapps/common/ForzaHorizon6/
PROTON_LOG=1 proton run forzahorizon6.exe &
sleep 5
```

### Step 2: Check Mod Initialization
Wait 10-15 seconds for the game to fully load, then check:

```bash
# Check if mod initialized successfully
cat ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
```

**Expected output** (if working):
```
2026-05-26 HH:MM:SS.SSS INFO  [bridge] FH6 Universal Radio starting...
2026-05-26 HH:MM:SS.SSS INFO  [bridge] running on port 8080
```

**Unexpected output** (if still failing):
```
ERROR [bridge] webui index.html missing or unreadable at ...
ERROR [bridge] aborting startup: webui credits/donation links check failed
```

### Step 3: Test Stuttering
- Play the game for 30 seconds
- **Does it stutter every second?** (Yes/No)
- **If no stutter**: Great! Test the radio functionality
- **If yes stutter**: Check the logs for repeated errors

### Step 4: Verify Radio Works
- Look for the radio station in the game UI
- Can you select it?
- Does audio play?
- Can you see the web dashboard (open http://127.0.0.1:8080 in browser)?

## Logs to Check

### Mod Log
```bash
tail -50 ~/.local/share/Steam/steamapps/common/ForzaHorizon6/fh6-radio/bridge.log
```

### Proton Log (errors around "version.dll")
```bash
grep -i "version.dll\|error\|crash" /home/devu/steam-2483190.log | tail -30
```

## Report Results
If you run the test, please share:
1. The output of bridge.log (first and last 10 lines)
2. Whether stuttering still occurs (yes/no)
3. Whether the radio station appears in game (yes/no)
4. Any errors from Proton log related to version.dll
