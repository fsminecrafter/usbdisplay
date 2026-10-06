<#
.SYNOPSIS
  Send text to the Pico USB display - one-shot, piped, or as an interactive prompt.

.DESCRIPTION
  Run it with no arguments to get a prompt:

      >>> send text (shift enter for newline, enter to send)

  Enter sends what you typed to the display. Shift+Enter starts a new line
  (Ctrl+Enter, Alt+Enter and Ctrl+J do the same). Up/Down browse earlier
  messages, Esc clears the input.

  Commands (typed alone on the prompt, any case):

      clear        clear the console AND the display
      exit         quit (also: quit, or Ctrl+C on an empty prompt)
      --copy       copy mode: what you type is run as a PowerShell command here,
                   and everything it prints shows up on the terminal AND on the
                   display. Type --copy again (or --copy off) to leave it.
                   "--copy <command>" mirrors just that one command.
      help         show this list

  To send the word "clear" itself, put a backslash in front: \clear

  Copy mode runs commands that finish on their own (dir, ipconfig, ping, git status).
  Programs that wait for keyboard input (python, ssh, edit) will not work in it.
  Ctrl+C while a mirrored command is running stops the whole script.

.EXAMPLE
  .\send.ps1
  Interactive prompt.
.EXAMPLE
  .\send.ps1 -Copy
  Interactive prompt that starts in copy mode.
.EXAMPLE
  .\send.ps1 "Hello from Windows"
.EXAMPLE
  .\send.ps1 -Clear "Fresh screen"
.EXAMPLE
  Get-Date | .\send.ps1
.EXAMPLE
  .\send.ps1 -File .\notes.txt -Port COM7
.EXAMPLE
  .\send.ps1 -Clear            # just clear the screen
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromPipeline = $true)]
    [string[]]$Text,

    [string]$File,
    [string]$Port,
    [switch]$Clear,
    [switch]$NoNewline,
    [switch]$Interactive,   # force the prompt even when other arguments are given
    [switch]$Copy           # start the prompt in copy mode (see above)
)

begin {
    $lines = New-Object System.Collections.Generic.List[string]
    $expectingInput = $MyInvocation.ExpectingInput

    $script:Sp           = $null
    $script:ExplicitPort = $Port
    $script:PortName     = $null
    $script:DisplayCols  = 80
    $script:DisplayDown  = $false
    $script:EditorResult = $null

    # Characters the 8x16 ASCII font cannot show, mapped to something close.
    $script:Translit = New-Object 'System.Collections.Generic.Dictionary[char,string]'
    foreach ($pair in @(
            @(0x00A0, ' '), @(0x2018, "'"), @(0x2019, "'"), @(0x201A, ','),
            @(0x201C, '"'), @(0x201D, '"'), @(0x201E, '"'),
            @(0x2013, '-'), @(0x2014, '-'), @(0x2212, '-'), @(0x2026, '...'),
            @(0x2022, '*'), @(0x00D7, 'x'), @(0x20AC, 'EUR'),
            @(0x00DF, 'ss'), @(0x00E6, 'ae'), @(0x00C6, 'AE'),
            @(0x00F8, 'o'), @(0x00D8, 'O'), @(0x0111, 'd'), @(0x0141, 'L'), @(0x0142, 'l'))) {
        $script:Translit[[char]$pair[0]] = $pair[1]
    }
    $script:BoxH = @(0x2500, 0x2501, 0x2504, 0x2505, 0x2508, 0x2509, 0x254C, 0x254D, 0x2550)
    $script:BoxV = @(0x2502, 0x2503, 0x2506, 0x2507, 0x250A, 0x250B, 0x254E, 0x254F, 0x2551)

    # The display font is plain ASCII: strip accents (a-ring -> a), map a few common
    # symbols (quotes, dashes, box drawing), everything else -> '?'.
    function ConvertTo-DisplayAscii([string]$s) {
        if ([string]::IsNullOrEmpty($s)) { return '' }
        if ($s -cmatch '^[\x20-\x7E\t\n]*$') { return $s }      # already plain ASCII
        # colour / title escape sequences from other programs: drop them
        $s = [regex]::Replace($s, '\x1B\[[0-?]*[ -/]*[@-~]', '')
        $s = [regex]::Replace($s, '\x1B\][^\x07\x1B]*(\x07|\x1B\\)', '')
        $s = $s.Normalize([Text.NormalizationForm]::FormD)
        $sb = New-Object Text.StringBuilder
        foreach ($ch in $s.ToCharArray()) {
            $code = [int]$ch
            if ($code -ge 32 -and $code -lt 127) { [void]$sb.Append($ch); continue }
            if ($code -eq 10 -or $code -eq 9 -or $code -eq 8 -or $code -eq 12) { [void]$sb.Append($ch); continue }
            if ($code -lt 32 -or $code -eq 127) { continue }    # CR and other control characters
            if ([Globalization.CharUnicodeInfo]::GetUnicodeCategory($ch) -eq 'NonSpacingMark') { continue }
            if ([char]::IsLowSurrogate($ch)) { continue }       # 2nd half of an emoji: one '?' is enough
            $rep = $null
            if ($script:Translit.TryGetValue($ch, [ref]$rep)) { [void]$sb.Append($rep); continue }
            if ($code -ge 0x2500 -and $code -le 0x257F) {
                if ($script:BoxH -contains $code) { [void]$sb.Append('-') }
                elseif ($script:BoxV -contains $code) { [void]$sb.Append('|') }
                else { [void]$sb.Append('+') }
                continue
            }
            if ($code -ge 0x2580 -and $code -le 0x259F) { [void]$sb.Append('#'); continue }
            [void]$sb.Append('?')
        }
        $sb.ToString()
    }

    function Find-DisplayPort {
        # Pico running the usbdisplay firmware shows up as "USB Serial Device (COMx)", VID 2E8A.
        $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.PNPDeviceID -like 'USB\VID_2E8A*' -and $_.Name -match '\((COM\d+)\)' } |
            Select-Object -First 1
        if ($dev) { [void]($dev.Name -match '\((COM\d+)\)'); return $Matches[1] }
        $names = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($names.Count -eq 1) { return $names[0] }
        return $null
    }

    function Close-Display {
        if ($script:Sp) {
            try { if ($script:Sp.IsOpen) { $script:Sp.Close() } } catch { }
            try { $script:Sp.Dispose() } catch { }
            $script:Sp = $null
        }
    }

    function Open-Display {
        $name = $script:ExplicitPort
        if (-not $name) { $name = Find-DisplayPort }
        if (-not $name) { throw "No Pico serial port found. Plug it in or pass -Port COMx." }
        $p = New-Object System.IO.Ports.SerialPort $name, 115200, 'None', 8, 'One'
        $p.DtrEnable    = $true      # the Pico only reads while DTR is asserted
        $p.WriteTimeout = 5000
        try { $p.Open() } catch { $p.Dispose(); throw }
        $script:Sp = $p
        $script:PortName = $name
    }

    # Convert to ASCII and write to the display. Reconnects once if the port went away
    # (e.g. the Pico was re-plugged or re-flashed).
    function Send-Display([string]$s) {
        if ([string]::IsNullOrEmpty($s)) { return }
        $bytes = [Text.Encoding]::ASCII.GetBytes((ConvertTo-DisplayAscii $s))
        $i = 0
        $retried = $false
        while ($i -lt $bytes.Length) {
            try {
                if (-not $script:Sp -or -not $script:Sp.IsOpen) { Open-Display }
                $n = [Math]::Min(256, $bytes.Length - $i)
                $script:Sp.Write($bytes, $i, $n)
                $i += $n
            }
            catch {
                if ($retried) { throw }
                $retried = $true
                Close-Display
                Start-Sleep -Milliseconds 300
            }
        }
    }

    # Like Send-Display but never throws: prints the problem once and skips the rest of
    # the current command's output.
    function Send-DisplaySafe([string]$s) {
        if ($script:DisplayDown) { return }
        try { Send-Display $s }
        catch {
            $script:DisplayDown = $true
            Write-Host ("display error: " + $_.Exception.Message) -ForegroundColor Red
        }
    }

    # ENQ (0x05): the firmware answers "USBDISPLAY 1 80x30 ...". $null if nothing answers.
    function Get-DisplayInfo {
        try {
            $script:Sp.ReadTimeout = 500
            $script:Sp.DiscardInBuffer()
            $script:Sp.Write([byte[]]@(5), 0, 1)
            $reply = $script:Sp.ReadLine()
            $script:Sp.DiscardInBuffer()
            return $reply.Trim()
        }
        catch { return $null }
    }

    function Show-Help {
        Write-Host 'Enter sends, Shift+Enter adds a new line (Ctrl+Enter / Ctrl+J also work).'
        Write-Host 'Up/Down: earlier messages (or move between lines).  Esc: clear the input.'
        Write-Host ''
        Write-Host '  clear         clear the console and the display'
        Write-Host '  exit          quit (also: quit, or Ctrl+C on an empty prompt)'
        Write-Host '  --copy        copy mode: what you type is run as a PowerShell command and its'
        Write-Host '                output shows up on the terminal AND the display. Again = leave.'
        Write-Host '  --copy off    leave copy mode      --copy <cmd>   mirror one command only'
        Write-Host '  help          this text'
        Write-Host '  \clear        send the word "clear" instead of clearing'
    }

    # Line editor: Enter returns the text, Shift+Enter inserts a newline. The result goes
    # into $script:EditorResult ($null = the user quit with Ctrl+C on an empty prompt).
    # Drawing: each logical line is "<prompt><text>", hard-wrapped to (width - 1) columns
    # so no row ever touches the last column - that keeps cursor maths identical on every
    # console, whatever its end-of-line wrap behaviour.
    function Read-MultiLine {
        param([string]$Prompt, [string]$ContPrompt, [string]$Placeholder, $History)

        $script:EditorResult = $null
        $sb = New-Object System.Text.StringBuilder
        $pos = 0
        $histIdx = $History.Count
        $draft = ''
        $done = $false
        $final = $false
        $prevRows = 1
        $ctrlC0 = [Console]::TreatControlCAsInput

        if ([Console]::CursorLeft -ne 0) { [Console]::WriteLine() }
        $top = [Console]::CursorTop

        $draw = {
            $W = [Math]::Min([Console]::BufferWidth, [Console]::WindowWidth)
            if ($W -lt 20) { $W = 20 }
            $cw = $W - 1
            $text = $sb.ToString()
            $before = $text.Substring(0, $pos)
            $cLine = $before.Split([char]10).Count - 1
            $cOff = $before.Length - ($before.LastIndexOf([char]10) + 1)
            $showPh = ($text.Length -eq 0) -and (-not $final) -and $Placeholder

            $rows = New-Object System.Collections.Generic.List[string]
            $cRow = 0
            $cCol = 0
            $logical = $text.Split([char]10)
            for ($li = 0; $li -lt $logical.Count; $li++) {
                if ($li -eq 0) { $pre = $Prompt } else { $pre = $ContPrompt }
                $full = $pre + $logical[$li]
                $n = $full.Length
                $nrows = [int][Math]::Max(1, [Math]::Ceiling($n / $cw))
                if ($li -eq $cLine) {
                    $cIdx = $pre.Length + $cOff
                    if ($cIdx -ge $nrows * $cw) { $nrows++ }     # cursor sits on a fresh wrapped row
                    $cRow = $rows.Count + [int][Math]::Floor($cIdx / $cw)
                    $cCol = $cIdx % $cw
                }
                for ($k = 0; $k -lt $nrows; $k++) {
                    $start = $k * $cw
                    if ($start -ge $n) { $rows.Add('') }
                    else { $rows.Add($full.Substring($start, [Math]::Min($cw, $n - $start))) }
                }
            }

            try { [Console]::CursorVisible = $false } catch { }
            [Console]::SetCursorPosition(0, $top)
            for ($j = 0; $j -lt $rows.Count; $j++) {
                if ($showPh -and $j -eq 0) {
                    $room = [Math]::Max(0, $cw - $Prompt.Length)
                    $ph = $Placeholder
                    if ($ph.Length -gt $room) { $ph = $ph.Substring(0, $room) }
                    [Console]::Write($Prompt)
                    $oldFg = [Console]::ForegroundColor
                    [Console]::ForegroundColor = [ConsoleColor]::DarkGray
                    [Console]::Write($ph)
                    [Console]::ForegroundColor = $oldFg
                    [Console]::Write(' ' * [Math]::Max(0, $room - $ph.Length))
                }
                else {
                    [Console]::Write($rows[$j].PadRight($cw))
                }
                if ($j -lt $rows.Count - 1) { [Console]::Write("`r`n") }     # scrolls at the bottom
            }
            # If the console scrolled, the cursor row tells us where the block starts now.
            $top = [Math]::Max(0, [Console]::CursorTop - ($rows.Count - 1))
            # Blank rows left over from a taller previous draw.
            for ($j = $rows.Count; $j -lt $prevRows; $j++) {
                try {
                    [Console]::SetCursorPosition(0, $top + $j)
                    [Console]::Write(' ' * $cw)
                } catch { }
            }
            $prevRows = $rows.Count
            [Console]::SetCursorPosition($cCol, $top + $cRow)
            try { [Console]::CursorVisible = $true } catch { }
        }

        [Console]::TreatControlCAsInput = $true      # Ctrl+C arrives as a key, we handle it
        try {
            . $draw
            while (-not $done) {
                $key = [Console]::ReadKey($true)
                # Handle every key that is already waiting (a paste) before redrawing once.
                while ($true) {
                    $k = $key.Key
                    $ch = $key.KeyChar
                    $mods = $key.Modifiers
                    $shift = [bool]($mods -band [ConsoleModifiers]::Shift)
                    $ctrl = [bool]($mods -band [ConsoleModifiers]::Control)
                    $alt = [bool]($mods -band [ConsoleModifiers]::Alt)
                    $text = $sb.ToString()

                    if ($ch -eq [char]3) {                                  # Ctrl+C
                        if ($sb.Length -gt 0) { [void]$sb.Clear(); $pos = 0 }
                        else { $done = $true; $script:EditorResult = $null }
                    }
                    elseif ($k -eq [ConsoleKey]::Enter -or $ch -eq [char]10) {
                        # A newline that arrives while more keys are waiting is part of a paste.
                        if ($shift -or $ctrl -or $alt -or $ch -eq [char]10 -or [Console]::KeyAvailable) {
                            [void]$sb.Insert($pos, [char]10)
                            $pos++
                        }
                        else { $done = $true; $script:EditorResult = $text }
                    }
                    elseif ($k -eq [ConsoleKey]::Backspace) {
                        if ($pos -gt 0) { [void]$sb.Remove($pos - 1, 1); $pos-- }
                    }
                    elseif ($k -eq [ConsoleKey]::Delete) {
                        if ($pos -lt $sb.Length) { [void]$sb.Remove($pos, 1) }
                    }
                    elseif ($k -eq [ConsoleKey]::LeftArrow) {
                        if ($ctrl) {
                            $p = $pos
                            while ($p -gt 0 -and [char]::IsWhiteSpace($text[$p - 1])) { $p-- }
                            while ($p -gt 0 -and -not [char]::IsWhiteSpace($text[$p - 1])) { $p-- }
                            $pos = $p
                        }
                        elseif ($pos -gt 0) { $pos-- }
                    }
                    elseif ($k -eq [ConsoleKey]::RightArrow) {
                        if ($ctrl) {
                            $p = $pos
                            while ($p -lt $text.Length -and -not [char]::IsWhiteSpace($text[$p])) { $p++ }
                            while ($p -lt $text.Length -and [char]::IsWhiteSpace($text[$p])) { $p++ }
                            $pos = $p
                        }
                        elseif ($pos -lt $sb.Length) { $pos++ }
                    }
                    elseif ($k -eq [ConsoleKey]::Home) {
                        if ($pos -gt 0) { $pos = $text.LastIndexOf([char]10, $pos - 1) + 1 }
                    }
                    elseif ($k -eq [ConsoleKey]::End) {
                        $e = $text.IndexOf([char]10, $pos)
                        if ($e -lt 0) { $e = $text.Length }
                        $pos = $e
                    }
                    elseif ($k -eq [ConsoleKey]::UpArrow) {
                        if ($pos -gt 0) { $ls = $text.LastIndexOf([char]10, $pos - 1) + 1 } else { $ls = 0 }
                        if ($ls -gt 0) {                                      # move up inside the message
                            $col = $pos - $ls
                            if ($ls -le 1) { $pls = 0 } else { $pls = $text.LastIndexOf([char]10, $ls - 2) + 1 }
                            $pos = $pls + [Math]::Min($col, ($ls - 1) - $pls)
                        }
                        elseif ($histIdx -gt 0) {                             # earlier message
                            if ($histIdx -eq $History.Count) { $draft = $text }
                            $histIdx--
                            [void]$sb.Clear(); [void]$sb.Append($History[$histIdx]); $pos = $sb.Length
                        }
                    }
                    elseif ($k -eq [ConsoleKey]::DownArrow) {
                        $le = $text.IndexOf([char]10, $pos)
                        if ($le -ge 0) {                                      # move down inside the message
                            if ($pos -gt 0) { $ls = $text.LastIndexOf([char]10, $pos - 1) + 1 } else { $ls = 0 }
                            $col = $pos - $ls
                            $nls = $le + 1
                            $nle = $text.IndexOf([char]10, $nls)
                            if ($nle -lt 0) { $nle = $text.Length }
                            $pos = $nls + [Math]::Min($col, $nle - $nls)
                        }
                        elseif ($histIdx -lt $History.Count) {                # later message / the draft
                            $histIdx++
                            if ($histIdx -eq $History.Count) { $h = $draft } else { $h = $History[$histIdx] }
                            [void]$sb.Clear(); [void]$sb.Append($h); $pos = $sb.Length
                        }
                    }
                    elseif ($k -eq [ConsoleKey]::Escape) {
                        [void]$sb.Clear(); $pos = 0
                    }
                    elseif ($k -eq [ConsoleKey]::Tab) {
                        if ($pos -gt 0) { $ls = $text.LastIndexOf([char]10, $pos - 1) + 1 } else { $ls = 0 }
                        $spaces = 4 - (($pos - $ls) % 4)
                        [void]$sb.Insert($pos, (' ' * $spaces)); $pos += $spaces
                    }
                    elseif ([int]$ch -ne 0 -and -not [char]::IsControl($ch)) {
                        [void]$sb.Insert($pos, $ch)
                        $pos++
                    }

                    if ($done -or -not [Console]::KeyAvailable) { break }
                    $key = [Console]::ReadKey($true)
                }
                if ($done) { $final = $true }
                . $draw
            }
            # Leave the cursor on the row below the input.
            try { [Console]::SetCursorPosition(0, $top + $prevRows - 1) } catch { }
            [Console]::WriteLine()
        }
        finally {
            [Console]::TreatControlCAsInput = $ctrlC0
            try { [Console]::CursorVisible = $true } catch { }
        }
    }
}

process {
    if ($Text) { foreach ($t in $Text) { $lines.Add($t) } }
}

end {
    # ".\send.ps1 --copy" arrives as ordinary text; treat it like -Copy.
    if ($lines.Count -eq 1 -and $lines[0] -eq '--copy') { $Copy = $true; $lines.Clear() }

    $hasPayload = ($lines.Count -gt 0) -or [bool]$File
    $noArgs = (-not $hasPayload) -and (-not $Clear) -and (-not $expectingInput)
    $stdinRedirected = [Console]::IsInputRedirected
    $wantPrompt = $Interactive -or $Copy -or ($noArgs -and -not $stdinRedirected)

    # ---- build what to send ----------------------------------------------------------
    $payload = ''
    if ($Clear) { $payload += [string][char]12 }                 # form feed = clear screen

    $body = $null
    if ($File) { $body = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $File).ProviderPath) }
    elseif ($lines.Count) {
        $body = ($lines -join "`n")
        if (-not $NoNewline) { $body += "`n" }
    }
    elseif ($noArgs -and $stdinRedirected -and -not $wantPrompt) {
        $body = [Console]::In.ReadToEnd()                        # some-command | powershell -File send.ps1
    }
    if ($body) { $payload += $body }

    # ---- one-shot --------------------------------------------------------------------
    if (-not $wantPrompt) {
        if (-not $payload) { Write-Warning "Nothing to send."; return }
        try {
            Open-Display
            Send-Display $payload
            $script:Sp.BaseStream.Flush()
            Start-Sleep -Milliseconds 150
        }
        finally { Close-Display }
        return
    }

    # ---- interactive prompt ----------------------------------------------------------
    if ($stdinRedirected) { throw "The interactive prompt needs a real console (input is redirected)." }

    try {
        Open-Display
        if ($Clear) { Clear-Host }
        if ($payload) { Send-Display $payload }

        $info = Get-DisplayInfo
        if ($info -match '^USBDISPLAY\s+\d+\s+(\d+)x(\d+)') {
            $script:DisplayCols = [int]$Matches[1]
            $what = " (display $($Matches[1])x$($Matches[2]))"
        }
        else { $what = ' (no ID reply from the display: old firmware or wrong port?)' }
        Write-Host "Connected to $($script:PortName)$what"
        Write-Host 'Commands: clear | exit | --copy | help' -ForegroundColor DarkGray
        if ($Copy) {
            Write-Host 'Copy mode: commands run here and their output shows on both. "--copy" again to leave.' -ForegroundColor DarkGray
        }

        $history = New-Object System.Collections.Generic.List[string]
        $copyMode = [bool]$Copy
        $mk = [string][char]0xE000          # marks lines that came from error/warning streams

        while ($true) {
            if ($copyMode) {
                $prompt = 'PS ' + (Get-Location).Path + '> '
                $cont = '>> '
                $hint = 'run a command: output goes to both screens (--copy to stop)'
            }
            else {
                $prompt = '>>> '
                $cont = '... '
                $hint = 'send text (shift enter for newline, enter to send)'
            }

            Read-MultiLine -Prompt $prompt -ContPrompt $cont -Placeholder $hint -History $history
            $entry = $script:EditorResult
            if ($null -eq $entry) { break }                      # Ctrl+C on an empty prompt

            $script:DisplayDown = $false
            if ($entry.Trim() -ne '' -and ($history.Count -eq 0 -or $history[$history.Count - 1] -ne $entry)) {
                $history.Add($entry)
            }
            $trim = $entry.Trim()
            $lc = $trim.ToLowerInvariant()

            # --copy [on|off|<command>]
            $oneShot = $false
            $m = [regex]::Match($trim, '^--copy(?:\s+(.*))?$', 'IgnoreCase, Singleline')
            if ($m.Success) {
                $arg = $m.Groups[1].Value.Trim()
                if ($arg -eq '' -or $arg -eq 'on' -or $arg -eq 'off') {
                    if ($arg -eq 'on') { $copyMode = $true }
                    elseif ($arg -eq 'off') { $copyMode = $false }
                    else { $copyMode = -not $copyMode }
                    if ($copyMode) {
                        Write-Host 'Copy mode ON: commands run here and their output shows on the terminal and the display.' -ForegroundColor DarkGray
                        Write-Host '--copy again (or --copy off) to go back to sending text.' -ForegroundColor DarkGray
                    }
                    else { Write-Host 'Copy mode OFF: Enter sends text again.' -ForegroundColor DarkGray }
                    continue
                }
                $entry = $arg
                $oneShot = $true
                $prompt = 'PS ' + (Get-Location).Path + '> '      # mirrored like a normal command
                $cont = '>> '
                $trim = $entry.Trim()
                $lc = $trim.ToLowerInvariant()
            }

            if ($lc -eq 'exit' -or $lc -eq 'quit') { break }
            if ($lc -eq 'clear' -or $lc -eq 'cls' -or $lc -eq 'clear-host') {
                Clear-Host
                Send-DisplaySafe ([string][char]12)
                continue
            }
            if ($lc -eq 'help' -or $lc -eq '--help') { Show-Help; continue }

            if ($copyMode -or $oneShot) {
                # Mirror the prompt + command exactly as the terminal shows them ...
                Send-DisplaySafe ($prompt + $entry.Replace("`n", "`n" + $cont) + "`n")
                if ($trim -eq '') { continue }
                $block = $null
                try { $block = [scriptblock]::Create($entry) }
                catch {
                    Write-Host $_.Exception.Message -ForegroundColor Red
                    Send-DisplaySafe ($_.Exception.Message + "`n")
                    continue
                }
                # ... then run it in this scope (variables and cd persist) and tee the output.
                try {
                    . $block *>&1 |
                        ForEach-Object {
                            if ($_ -is [System.Management.Automation.ErrorRecord]) {
                                foreach ($l in ($_.ToString() -split "\r?\n")) { $mk + 'E' + $l }
                            }
                            elseif ($_ -is [System.Management.Automation.WarningRecord]) {
                                foreach ($l in (('WARNING: ' + $_.Message) -split "\r?\n")) { $mk + 'W' + $l }
                            }
                            elseif ($_ -is [System.Management.Automation.VerboseRecord]) { 'VERBOSE: ' + $_.Message }
                            elseif ($_ -is [System.Management.Automation.DebugRecord]) { 'DEBUG: ' + $_.Message }
                            elseif ($_ -is [System.Management.Automation.InformationRecord]) { [string]$_.MessageData }
                            else { $_ }
                        } |
                        Out-String -Stream -Width $script:DisplayCols |
                        ForEach-Object {
                            $line = [string]$_
                            $color = $null
                            if ($line.StartsWith($mk, [StringComparison]::Ordinal) -and $line.Length -ge 2) {
                                if ($line[1] -eq 'E') { $color = 'Red' } else { $color = 'Yellow' }
                                $line = $line.Substring(2)
                            }
                            if ($color) { Write-Host $line -ForegroundColor $color } else { Write-Host $line }
                            Send-DisplaySafe ($line.TrimEnd() + "`n")
                        }
                }
                catch {
                    Write-Host $_.Exception.Message -ForegroundColor Red
                    Send-DisplaySafe ($_.Exception.Message + "`n")
                }
                continue
            }

            # Plain text: send it, newline-terminated. "\clear" sends the word itself.
            $out = $entry
            if ($out.StartsWith('\')) {
                $w = $out.Substring(1).Trim().ToLowerInvariant()
                if ($w -in @('exit', 'quit', 'clear', 'cls', 'clear-host', 'help', '--help') -or $w -match '^--copy(\s|$)') {
                    $out = $out.Substring(1)
                }
            }
            Send-DisplaySafe ($out + "`n")
        }
    }
    finally {
        Close-Display
        try { [Console]::CursorVisible = $true } catch { }
    }
}
