#!/bin/bash
# guard-zsh-scalar-command.sh — PreToolUse(Bash) guard.
#
# Blocks the recurring zsh word-splitting footgun: assigning a MULTI-WORD value
# to a scalar and then expanding it UNQUOTED as a command or as flags, e.g.
#     IM="ssh -o X host"; $IM cmd            # zsh runs the whole string as ONE
#     SSHOPT="-o A -o B"; ssh $SSHOPT host   # command name / ONE arg, and fails
# The Bash tool's shell here is zsh, which — unlike bash/sh — does NOT word-split
# an unquoted parameter expansion. The failure is silent/misleading (command
# runs but does the wrong thing), and it keeps recurring under new variable
# names (IM, IO, SO, RSH, ...), so a doc rule alone doesn't hold. This enforces.
#
# Design notes:
#   * Bash-tool shell state does NOT persist between calls, so the assignment and
#     the misuse always live in the SAME command string -> single-string
#     analysis is complete.
#   * Only MULTI-WORD scalars can trigger the bug (single-word never splits), and
#     we only flag them when used unquoted in COMMAND position, or when the value
#     is a command / flags string. This leaves normal `echo $msg`, `for f in
#     $files`, `cp $src $dst` untouched — no false positives on data vars.
#   * Fails OPEN (exit 0) if python3 is unavailable or input is unparseable — a
#     guard must never block work because the guard itself couldn't run.
#
# Exit codes: 0 = allow, 2 = block (message on stderr).

input=$(cat)
command -v python3 >/dev/null 2>&1 || exit 0

HOOK_INPUT="$input" python3 <<'PY'
import os, json, re, sys

try:
    data = json.loads(os.environ.get("HOOK_INPUT", "") or "{}")
except Exception:
    sys.exit(0)

if data.get("tool_name") != "Bash":
    sys.exit(0)
cmd = (data.get("tool_input") or {}).get("command", "") or ""
if not cmd:
    sys.exit(0)

EXEC_HEADS = ("ssh", "rsync", "scp", "sshpass", "docker", "pct", "kubectl",
              "curl", "wget", "sudo", "env", "sh", "bash", "fw_setenv", "adb")
WORD = r"[A-Za-z_][A-Za-z0-9_]*"


def classify(val):
    v = val.strip()
    inner = v
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        inner = v[1:-1]
    multiword = (" " in inner) or ("\t" in inner)
    subst = v.startswith("$(") or v.startswith("`")
    head = inner.strip().split(" ", 1)[0] if inner.strip() else ""
    is_exec = head in EXEC_HEADS
    is_flags = inner.strip().startswith("-")
    # Multi-word (or command-substitution) is the required gate: a single-word
    # scalar never triggers zsh's split footgun. exec/flags only refine WHERE.
    if multiword or subst:
        return {"exec": is_exec, "flags": is_flags}
    return None


def consume_value(s, j):
    n = len(s)
    start = j
    while j < n:
        c = s[j]
        if c == "'":
            j += 1
            while j < n and s[j] != "'":
                j += 1
            j += 1
            continue
        if c == '"':
            j += 1
            while j < n and s[j] != '"':
                if s[j] == "\\":
                    j += 1
                j += 1
            j += 1
            continue
        if c == "$" and j + 1 < n and s[j + 1] == "(":
            depth = 0
            j += 1
            while j < n:
                if s[j] == "(":
                    depth += 1
                elif s[j] == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            continue
        if c in " \t;\n&|":
            break
        j += 1
    return s[start:j], j


def scan(cmd):
    risky = {}
    hits = []
    i, n = 0, len(cmd)
    in_s = in_d = False
    cmd_pos = True

    while i < n:
        c = cmd[i]

        if in_s:
            if c == "'":
                in_s = False
            i += 1
            continue
        if in_d:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_d = False
                i += 1
                continue
            i += 1
            continue

        if c == "\\":
            i += 2
            continue
        if c == "'":
            in_s = True
            cmd_pos = False
            i += 1
            continue
        if c == '"':
            in_d = True
            cmd_pos = False
            i += 1
            continue
        if c in ";\n&|(){":
            cmd_pos = True
            i += 1
            continue
        if c in " \t":
            i += 1
            continue

        if cmd_pos:
            m = re.match(r"(%s)=(?!=)" % WORD, cmd[i:])
            if m:
                name = m.group(1)
                val, j = consume_value(cmd, i + m.end())
                cls = classify(val)
                if cls:
                    risky[name] = cls
                i = j
                continue

        if c == "$":
            gm = re.match(r"\$\{(=?)(%s)" % WORD, cmd[i:])
            if gm:
                split_op, name = gm.group(1), gm.group(2)
                m_end = re.match(r"\$\{=?%s\}?" % WORD, cmd[i:]).end()
            else:
                sm = re.match(r"\$(%s)" % WORD, cmd[i:])
                if not sm:
                    cmd_pos = False
                    i += 1
                    continue
                split_op, name, m_end = "", sm.group(1), sm.end()
            if name in risky and split_op != "=":
                cls = risky[name]
                if cmd_pos:
                    hits.append((name, "expanded unquoted in COMMAND position"))
                elif cls["exec"]:
                    hits.append((name, "holds a command and is expanded unquoted"))
                elif cls["flags"]:
                    hits.append((name, "holds flags, expanded unquoted (zsh "
                                       "keeps them as ONE arg)"))
            cmd_pos = False
            i += m_end
            continue

        cmd_pos = False
        i += 1

    return hits


hits = scan(cmd)
if not hits:
    sys.exit(0)

seen = []
for name, reason in hits:
    line = "$%s %s" % (name, reason)
    if line not in seen:
        seen.append(line)

sys.stderr.write("BLOCK (zsh word-splitting footgun):\n")
for l in seen:
    sys.stderr.write("  - %s\n" % l)
sys.stderr.write(
    "\nThe Bash tool runs zsh, which does NOT word-split an unquoted $VAR, so a\n"
    "scalar holding a command/flags runs as ONE mangled argument and fails\n"
    "silently. Fix, in order of preference:\n"
    "  1. Inline the command/flags directly (no scalar).\n"
    "  2. Use a function that forwards args: f() { ssh -o X host \"$@\"; }\n"
    "  3. If you truly want splitting: ${=VAR} (zsh) or run the snippet under sh.\n")
sys.exit(2)
PY
exit $?
