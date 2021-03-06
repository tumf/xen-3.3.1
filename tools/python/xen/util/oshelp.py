import fcntl
import os

def fcntl_setfd_cloexec(file, bool):
        f = fcntl.fcntl(file, fcntl.F_GETFD)
        if bool: f |= fcntl.FD_CLOEXEC
        else: f &= ~fcntl.FD_CLOEXEC
        fcntl.fcntl(file, fcntl.F_SETFD)

def waitstatus_description(st):
        if os.WIFEXITED(st):
                es = os.WEXITSTATUS(st)
                if es: return "exited with nonzero status %i" % es
                else: return "exited"
        elif os.WIFSIGNALED(st):
                s = "died due to signal %i" % os.WTERMSIG(st)
                if os.WCOREDUMP(st): s += " (core dumped)"
                return s
        else:
                return "failed with unexpected wait status %i" % st
