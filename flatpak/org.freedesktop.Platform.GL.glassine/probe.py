# SPDX-License-Identifier: GPL-2.0-or-later
# Run inside a Flatpak sandbox by check.sh (python3 of the runtime, fed on stdin):
# create an EGL context on the surfaceless platform and a Vulkan instance, then print what
# they report and which Mesa libraries the process loaded. Without a GPU (a CI
# container) EGL is llvmpipe and Vulkan lavapipe; on a device EGL is freedreno.
import ctypes as C

egl = C.CDLL("libEGL.so.1")
egl.eglGetPlatformDisplay.restype = C.c_void_p
egl.eglGetPlatformDisplay.argtypes = [C.c_uint, C.c_void_p, C.c_void_p]
egl.eglInitialize.argtypes = [C.c_void_p, C.POINTER(C.c_int), C.POINTER(C.c_int)]
egl.eglQueryString.restype = C.c_char_p
egl.eglQueryString.argtypes = [C.c_void_p, C.c_int]
egl.eglBindAPI.argtypes = [C.c_uint]
egl.eglCreateContext.restype = C.c_void_p
egl.eglCreateContext.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.c_int)]
egl.eglMakeCurrent.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]

EGL_PLATFORM_SURFACELESS_MESA = 0x31DD
dpy = egl.eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, None, None)
major, minor = C.c_int(), C.c_int()
if not dpy or not egl.eglInitialize(dpy, C.byref(major), C.byref(minor)):
    print("egl: no display")
else:
    print("egl: vendor %s, EGL %d.%d" % (egl.eglQueryString(dpy, 0x3053).decode(), major.value, minor.value))
    egl.eglBindAPI(0x30A0)  # EGL_OPENGL_ES_API
    attribs = (C.c_int * 3)(0x3098, 2, 0x3038)  # EGL_CONTEXT_CLIENT_VERSION 2
    ctx = egl.eglCreateContext(dpy, None, None, attribs)  # EGL_KHR_no_config_context
    if not ctx or not egl.eglMakeCurrent(dpy, None, None, ctx):  # surfaceless
        print("egl: no GLES2 context")
    else:
        gl = C.CDLL("libGLESv2.so.2")
        gl.glGetString.restype = C.c_char_p
        gl.glGetString.argtypes = [C.c_uint]
        print("gl: renderer %s" % gl.glGetString(0x1F01).decode())
        print("gl: version %s" % gl.glGetString(0x1F02).decode())


class VkInstanceCreateInfo(C.Structure):
    _fields_ = [("sType", C.c_uint), ("pNext", C.c_void_p), ("flags", C.c_uint),
                ("pApplicationInfo", C.c_void_p), ("enabledLayerCount", C.c_uint),
                ("ppEnabledLayerNames", C.c_void_p), ("enabledExtensionCount", C.c_uint),
                ("ppEnabledExtensionNames", C.c_void_p)]


vk = C.CDLL("libvulkan.so.1")
vk.vkCreateInstance.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
vk.vkEnumeratePhysicalDevices.argtypes = [C.c_void_p, C.POINTER(C.c_uint), C.c_void_p]
vk.vkGetPhysicalDeviceProperties.argtypes = [C.c_void_p, C.c_void_p]
info = VkInstanceCreateInfo(sType=1)  # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
inst = C.c_void_p()
r = vk.vkCreateInstance(C.byref(info), None, C.byref(inst))
if r != 0:
    print("vulkan: vkCreateInstance returned %d" % r)
else:
    n = C.c_uint()
    vk.vkEnumeratePhysicalDevices(inst, C.byref(n), None)
    devs = (C.c_void_p * max(n.value, 1))()
    vk.vkEnumeratePhysicalDevices(inst, C.byref(n), devs)
    if n.value == 0:
        print("vulkan: no physical device")
    for i in range(n.value):
        props = C.create_string_buffer(4096)  # VkPhysicalDeviceProperties is ~830 bytes
        vk.vkGetPhysicalDeviceProperties(devs[i], props)
        api = int.from_bytes(props.raw[0:4], "little")
        name = props.raw[20:276].split(b"\0")[0].decode()  # deviceName after 5 uint32
        print("vulkan: device %s, Vulkan %d.%d" % (name, api >> 22 & 0x7F, api >> 12 & 0x3FF))

seen = set()
for line in open("/proc/self/maps"):
    path = line.split()[-1]
    if "/GL/" in path and path not in seen:
        seen.add(path)
        print("loaded: %s" % path)
