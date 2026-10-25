' TokenOrb proxy autostart for Windows (no admin needed).
'
' 1) Edit the two paths below: your pythonw.exe, and this repo's proxy script.
' 2) Pick a token and put the SAME value in PROXY_TOKEN below AND on the orb
'    (config portal -> "Proxy token", or firmware #define NET_PROXY_QUERY).
'    The proxy's security gate refuses LAN access when bound to 0.0.0.0 without
'    a token and silently falls back to 127.0.0.1 -- then the orb shows "NO PROXY".
' 3) Copy this file into your Startup folder:  Win+R  ->  shell:startup
'    It then launches the proxy hidden every time you log in.
'
Dim oShell, oEnv
Set oShell = CreateObject("WScript.Shell")
Set oEnv = oShell.Environment("PROCESS")   ' the launched python inherits these

' --- expose the proxy on the LAN so the orb can reach it ---
oEnv("BIND_HOST")   = "0.0.0.0"
oEnv("PROXY_TOKEN") = "orb123"             ' <-- CHANGE THIS (must match the orb)

oShell.Run """C:\Path\To\pythonw.exe"" ""C:\Path\To\TokenOrb\proxy\claude_limits_proxy.py""", 0, False
