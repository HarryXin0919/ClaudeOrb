' ClaudeOrb proxy autostart for Windows (no admin needed).
'
' 1) Edit the two paths below: your pythonw.exe, and this repo's proxy script.
' 2) Copy this file into your Startup folder:  Win+R  ->  shell:startup
'    It then launches the proxy hidden every time you log in.
'
CreateObject("WScript.Shell").Run """C:\Path\To\pythonw.exe"" ""C:\Path\To\ClaudeOrb\proxy\claude_limits_proxy.py""", 0, False
