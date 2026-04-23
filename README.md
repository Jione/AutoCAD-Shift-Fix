# AutoCAD Shift Key Fix (for Windows 11)

This repository provides an AutoHotkey script to resolve the issue where **Shift key temporary overrides** (e.g., Ortho mode toggle) fail to function in AutoCAD on Windows 11 or certain laptop environments.

## The Problem
In recent Windows 11 updates and some laptop power-saving firmwares, the Shift key only sends a single `key_down` signal without repeating. Since AutoCAD expects at least two `down` signals to recognize a "held" state, the temporary override function fails.

## The Solution
This script detects when the Shift key is held down and manually sends an additional `down` signal to satisfy AutoCAD's input requirements.

## How to Use
1. **Install AutoHotkey:** Download and install [AutoHotkey v1.1](https://www.autohotkey.com).
2. **Download the Script:** Download `AutoCAD_ShiftFix.ahk` from this repository.
3. **Run the Script:** Double-click the file to run it. You will see a green 'H' icon in your system tray.
4. **Auto-Start (Optional):** If you want it to run every time Windows starts, press `Win + R`, type `shell:startup`, and place a shortcut of the script in that folder.
