# POST build script: compile Mbed TLS, the SDK's hardware entropy source for it
# (pico_mbedtls), and lwIP's altcp_tls Mbed TLS port as libraries. The
# picosdk framework builds lwIP's core (altcp included) but not these.
Import("env")
from os.path import join

fw = env.PioPlatform().get_package_dir("framework-picosdk")
libs = [
    env.BuildLibrary(join("$BUILD_DIR", "MbedTLS"), join(fw, "lib", "mbedtls", "library"), "+<*.c>"),
    # lwIP's own altcp_tls_mbedtls.c predates Mbed TLS 3: the SDK carries a
    # patched copy in pico_lwip, used with lwIP's memory glue.
    env.BuildLibrary(join("$BUILD_DIR", "AltcpTlsMem"), join(fw, "lib", "lwip", "src", "apps", "altcp_tls"), "-<*> +<altcp_tls_mbedtls_mem.c>"),
    env.BuildLibrary(join("$BUILD_DIR", "AltcpTls"), join(fw, "src", "rp2_common", "pico_lwip"), "-<*> +<altcp_tls_mbedtls.c>"),
    env.BuildLibrary(join("$BUILD_DIR", "PicoMbedtls"), join(fw, "src", "rp2_common", "pico_mbedtls"), "+<pico_mbedtls.c>"),
]
env.Prepend(LIBS=libs)
