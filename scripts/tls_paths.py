# PRE build script: headers and config for Mbed TLS (the SDK's lib/mbedtls,
# 3.6.2) and lwIP's TLS layer, for every translation unit.
Import("env")
from os.path import join

fw = env.PioPlatform().get_package_dir("framework-picosdk")
env.Append(
    CPPPATH=[
        join(fw, "lib", "mbedtls", "include"),
        join(fw, "lib", "lwip", "src", "apps", "altcp_tls"),
        join(fw, "src", "rp2_common", "pico_mbedtls", "include"),
    ],
    CPPDEFINES=[("MBEDTLS_CONFIG_FILE", '\\"tls_mbedtls_config.h\\"')],
)
