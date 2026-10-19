#!/usr/bin/env python3
"""Spawn a command (wine + original exe) and serve reads of the game's memory on a unix socket.

Why a server: kernel.yama.ptrace_scope = 1 only lets an ANCESTOR read /proc/<pid>/mem, so the reader must be the
process that launched wine (we also become a child subreaper so double-forked wine processes stay descendants).
The originals have no ASLR (image base 0x400000, DllCharacteristics 0), so Ghidra VAs are live addresses.

usage: memsrv.py SOCKET EXE_NAME -- cmd...
protocol (one request line, one reply):  "R addr len [addr len ...]\n" -> u32 total length + raw bytes
                                          "P\n" -> u32 4 + u32 pid of the process whose cmdline contains EXE_NAME
"""
import ctypes, os, socket, struct, subprocess, sys, threading


def find_pid(exe):
    exe = exe.lower()
    for p in os.listdir('/proc'):
        if p.isdigit():
            try:
                cmd = open(f'/proc/{p}/cmdline', 'rb').read().lower()
            except OSError:
                continue
            # wine shows the Windows path as argv[0]; skip the "start.exe"/wine launcher wrappers
            if exe.encode() in cmd.split(b'\0')[0]:
                return int(p)
    return 0


def serve(conn, exe, state):
    f = conn.makefile('rwb')
    for line in f:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == b'P':
            state['pid'] = find_pid(exe)
            out = struct.pack('<I', state['pid'])
        elif parts[0] == b'R':
            if not state.get('pid') or not os.path.exists(f"/proc/{state['pid']}"):
                state['pid'] = find_pid(exe)
            out = b''
            try:
                with open(f"/proc/{state['pid']}/mem", 'rb', buffering=0) as m:
                    for a, n in zip(parts[1::2], parts[2::2]):
                        m.seek(int(a, 0))
                        out += m.read(int(n, 0))
            except (OSError, ValueError):
                out = b''
        else:
            out = b''
        f.write(struct.pack('<I', len(out)) + out)
        f.flush()


def main():
    sock, exe = sys.argv[1], sys.argv[2]
    cmd = sys.argv[sys.argv.index('--') + 1:]
    ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)  # PR_SET_CHILD_SUBREAPER
    if os.path.exists(sock):
        os.unlink(sock)
    s = socket.socket(socket.AF_UNIX)
    s.bind(sock)
    s.listen(8)
    proc = subprocess.Popen(cmd)
    state = {}

    def accept():
        while True:
            c, _ = s.accept()
            threading.Thread(target=serve, args=(c, exe, state), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    rc = proc.wait()
    # keep serving while other descendants (the real game process) are alive
    while find_pid(exe):
        try:
            os.waitpid(-1, 0)
        except ChildProcessError:
            break
    os.unlink(sock)
    sys.exit(rc)


if __name__ == '__main__':
    main()
