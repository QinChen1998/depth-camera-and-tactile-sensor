# Generate a private rkipc ini. The factory/user ini is an input only.
function set(section, key, value) { values[section SUBSEP key] = value }
function flush( key, parts) {
    for (key in values) {
        split(key, parts, SUBSEP)
        if (parts[1] == section && !(key in written)) {
            print parts[2] " = " values[key]
            written[key] = 1
        }
    }
}
BEGIN {
    if (runtime !~ /^\/[A-Za-z0-9_\/.-]+$/) { invalid = 1; exit 2 }
    set("video.source", "enable_aiq", "1")
    set("video.source", "enable_venc_0", "1")
    set("video.source", "enable_venc_1", "0")
    set("video.source", "enable_venc_2", "0")
    set("video.source", "enable_jpeg", "0")
    set("video.source", "enable_npu", "0")
    set("video.source", "enable_ivs", "0")
    set("video.source", "enable_vo", "0")
    set("video.source", "enable_rtsp", "1")
    set("video.source", "enable_rtmp", "0")
    set("video.source", "enable_wrap", "1")
    set("video.source", "buffer_line", "648")
    set("video.0", "width", "2304"); set("video.0", "max_width", "2304")
    set("video.0", "height", "1296"); set("video.0", "max_height", "1296")
    set("video.0", "src_frame_rate_num", "30"); set("video.0", "src_frame_rate_den", "1")
    set("video.0", "dst_frame_rate_num", "30"); set("video.0", "dst_frame_rate_den", "1")
    set("video.0", "output_data_type", "H.265")
    set("video.0", "rc_mode", "CBR"); set("video.0", "max_rate", "4096")
    set("video.0", "mid_rate", "2048"); set("video.0", "min_rate", "0")
    set("video.0", "gop", "30"); set("video.0", "gop_mode", "normalP")
    set("video.0", "enable_refer_buffer_share", "1")
    # Applying the saved ISP settings also touches IR-cut/PWM. Use the IQ defaults.
    set("isp", "init_form_ini", "0")
    set("audio.0", "enable", "0"); set("audio.0", "enable_aed", "0")
    set("audio.0", "enable_bcd", "0")
    set("osd.common", "enable_osd", "0")
    set("network.ntp", "enable", "0")
    set("storage", "mount_path", runtime "/storage")
    set("storage", "num_limit_enable", "0")
    set("storage.0", "enable", "0"); set("storage.1", "enable", "0")
    set("storage.2", "enable", "0")
    set("event.regional_invasion", "enabled", "0")
}
{
    sub(/\r$/, "")
    if ($0 ~ /^[ \t]*\[/) {
        flush()
        section = $0; sub(/^[ \t]*\[/, "", section); sub(/\][ \t]*(;.*)?$/, "", section)
        if (section ~ /\]/ || sections[section]++) invalid = 1
    } else if (index($0, "=") && $0 !~ /^[ \t]*[;#]/) {
        key = substr($0, 1, index($0, "=") - 1)
        gsub(/^[ \t]+|[ \t]+$/, "", key)
        entry = section SUBSEP key
        if (entry in values) {
            if (entry in written) invalid = 1
            print key " = " values[entry]
            written[entry] = 1
            next
        }
    }
    print
}
END {
    if (invalid || !("video.source" in sections) || !("video.0" in sections) || !("isp" in sections)) exit 2
    flush()
    for (entry in values) {
        if (!(entry in written)) {
            split(entry, parts, SUBSEP)
            section = parts[1]
            if (!(section in sections)) { print "\n[" section "]"; sections[section] = 1 }
            flush()
        }
    }
}
