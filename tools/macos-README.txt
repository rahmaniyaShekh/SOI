soi-share for macOS
===================

Serverless peer-to-peer screen sharing from the terminal. This folder is the
whole program: nothing to install, no admin rights, no Homebrew. One binary
runs on Apple silicon and Intel Macs, macOS 10.15 Catalina and later.


First run (once)
----------------

1. Open Terminal in this folder. In Finder: right-click the folder >
   New Terminal at Folder. Or in Terminal:

       cd ~/Downloads/soi-share

2. macOS marks downloaded programs as "from the internet" and blocks ones that
   are not notarized by Apple. Clear the mark on this one:

       xattr -d com.apple.quarantine soi-share

   (If you skip this, macOS says it "cannot be opened" or "could not verify"
   it. You can also allow it in System Settings > Privacy & Security, where
   an "Allow Anyway" button appears after the first attempt.)

3. Start sharing:

       ./soi-share start

   The first time, macOS asks for Screen Recording permission for your
   terminal app. Allow it in System Settings > Privacy & Security >
   Screen Recording (macOS 15: "Screen & System Audio Recording"), then quit
   the terminal app completely (Cmd+Q), open it again in this folder, and run
   ./soi-share start again.


Every day
---------

    ./soi-share start      share in the background; prints a 6-character code
    ./soi-share status     is it running? the code, live stats
    ./soi-share stop       stop sharing
    ./soi-share help       everything else

Your friend opens https://share.mdarif.online and types the code. The share
keeps running in the background after you close the Terminal window; stop it
with ./soi-share stop.


Optional
--------

    ./soi-share install    copy it to ~/.soi-share/bin and put that on your
                           PATH, so plain `soi-share` works in any terminal
    ./soi-share update     newest release, checksum-verified, replaced in place
    ./soi-share uninstall  undo install (your share code is kept unless --purge)

viewer.html is only for the offline --no-code flow; the binary already
contains it. THIRD_PARTY_NOTICES.md lists the libraries compiled in.
