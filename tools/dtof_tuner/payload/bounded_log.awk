# Two 64 KiB slots per process in /run; continue draining after rotation.
BEGIN { slot = 0; size = 0; path = base ".0"; printf "%s", "" > path }
{
    line = substr($0, 1, 4096)
    if (size + length(line) + 1 > 65536) {
        close(path)
        slot = 1 - slot
        path = base "." slot
        printf "%s", "" > path
        size = 0
    }
    print line > path
    fflush(path)
    size += length(line) + 1
}
