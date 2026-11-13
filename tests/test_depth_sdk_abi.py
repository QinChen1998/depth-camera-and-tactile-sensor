"""Exercise the Windows SDK from a process without Qt or device access."""

import argparse
import base64
import ctypes as c
import json
import os
from pathlib import Path
import struct
import threading


class Options(c.Structure):
    _fields_ = [("size", c.c_uint32), ("abi_version", c.c_uint32),
                ("runtime_directory", c.c_char_p), ("device_store", c.c_char_p)]


def write_registry(path):
    devices = []
    for number in (1, 2):
        key = struct.pack(">I", 11) + b"ssh-ed25519" + struct.pack(">I", 32) + bytes([number]) * 32
        devices.append({
            "id": f"fixture-{number}", "name": f"Offline device {number}",
            "host": "127.0.0.1", "ssh_port": 22, "serial": f"{number:016x}",
            "public_key": base64.b64encode(key).decode("ascii"), "username": "root",
            "calibration": "rv20261109-f6", "input_rotation": 0, "camera_mount_rotation": 0,
            "legacy_coordinator": True, "last_connected": "",
        })
    path.write_text(json.dumps({
        "schema": 1, "kind": "rv1103-devices", "active": "fixture-1", "devices": devices,
    }), encoding="utf-8")


def run(dll_path, work):
    dll = c.CDLL(str(dll_path))
    handle = c.c_void_p

    def bind(name, *types):
        function = getattr(dll, "DctsDepth_" + name)
        function.argtypes = types
        function.restype = c.c_int32
        return function

    abi = bind("AbiVersion")
    open_sdk = bind("Open", c.POINTER(Options), c.POINTER(handle))
    close = bind("Close", handle)
    select = bind("SelectDevice", handle, c.c_char_p)
    mode = bind("SetMode", handle, c.c_uint32)
    poll = bind("Poll", handle)
    stop = bind("Stop", handle)
    check_usb = bind("CheckUsb", handle)
    list_devices = bind("ListDevices", handle, c.c_void_p, c.POINTER(c.c_uint32))
    get_state = bind("GetState", handle, c.c_void_p, c.POINTER(c.c_uint32))

    def read(call, session):
        required = c.c_uint32(0)
        assert call(session, None, c.byref(required)) == 0 and required.value > 0
        small = c.create_string_buffer(1)
        capacity = c.c_uint32(1)
        assert call(session, small, c.byref(capacity)) == 5 and capacity.value == required.value
        output = c.create_string_buffer(required.value)
        assert call(session, output, c.byref(required)) == 0
        return json.loads(output.value)

    registry = work / "devices.json"
    write_registry(registry)
    before = registry.read_bytes()
    options = Options(c.sizeof(Options), 1, str(dll_path.parent).encode("utf-8"), str(registry).encode("utf-8"))
    session = handle()
    assert abi() == 1
    for cycle in range(2):
        print(f"SDK open cycle {cycle + 1}", flush=True)
        assert open_sdk(c.byref(options), c.byref(session)) == 0 and session.value
        try:
            other = handle()
            assert open_sdk(c.byref(options), c.byref(other)) == 2 and not other.value
            wrong_thread = []
            thread = threading.Thread(target=lambda: wrong_thread.append(poll(session)))
            thread.start()
            thread.join()
            assert wrong_thread == [3]
            devices = read(list_devices, session)
            assert [row["id"] for row in devices] == ["fixture-1", "fixture-2"]
            assert [row["selected"] for row in devices] == [True, False]
            assert select(session, b"unknown-device") == 4
            assert select(session, b"fixture-2") == 0
            assert mode(session, 2) == 0
            state = read(get_state, session)
            assert state["device_id"] == "fixture-2" and state["mode"] == 2 and state["user_stopped"]
            assert mode(session, 1) == 0 and poll(session) == 0
            assert check_usb(session) == 6
            assert stop(session) == 0 and registry.read_bytes() == before
        finally:
            assert close(session) == 0
        assert poll(session) == 1
    print("PASS SDK ABI, owner thread, one active session, device and mode selection, offline guard, and reopen")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qt-bin", type=Path)
    args = parser.parse_args()
    os.environ["RV_TEST_OFFLINE_ONLY"] = "1"
    args.output.mkdir(parents=True, exist_ok=False)
    dll = args.dll.resolve()
    with os.add_dll_directory(str((args.qt_bin or dll.parent).resolve())):
        run(dll, args.output.resolve())
