
set(SECURITY_DIR      ${ZEPHYR_HAL_SILABS_MODULE_DIR}/simplicity_sdk/platform/security/sl_component)
set(CRYPTO_DRIVER_DIR ${ZEPHYR_HAL_SILABS_MODULE_DIR}/simplicity_sdk/util/third_party/mbedtls)
set(MBEDTLS_DIR       ${ZEPHYR_MBEDTLS_MODULE_DIR})

if(CONFIG_MBEDTLS_PSA_CRYPTO_SILABS_DRIVER_WRAPPER)

zephyr_include_directories(
  inc
  ${CRYPTO_DRIVER_DIR}/include
)

zephyr_library_named(mbedTLSCryptoDriver)

target_include_directories(mbedTLSCryptoDriver PUBLIC
  ${CRYPTO_DRIVER_DIR}/library
  ${MBEDTLS_DIR}/library
)

zephyr_library_sources(
  ${CRYPTO_DRIVER_DIR}/library/psa_crypto_driver_wrappers_no_static.c
)

zephyr_compile_definitions_ifdef(CONFIG_SOC_GECKO_SE
  SL_SE_MANAGER_THREADING
)

set(vse_device_families 22 27)
if(CONFIG_SOC_FAMILY_SILABS_S2 AND NOT (SILABS_DEVICE_FAMILY_NUMBER IN_LIST vse_device_families))
  zephyr_compile_definitions_ifdef(CONFIG_SOC_GECKO_SE
    SL_SE_MANAGER_YIELD_WHILE_WAITING_FOR_COMMAND_COMPLETION
  )
endif()

# Shared files for all drivers
if(CONFIG_PSA_CRYPTO_DRIVER_SILABS_HSE)

zephyr_include_directories(
  ${SECURITY_DIR}/sl_psa_driver/inc
  ${SECURITY_DIR}/sl_mbedtls_support/config
  ${SECURITY_DIR}/sl_mbedtls_support/inc
)
zephyr_compile_definitions(
  MBEDTLS_PSA_CRYPTO_DRIVERS
)
zephyr_library_sources(
  ${SECURITY_DIR}/sl_psa_driver/src/sli_psa_driver_common.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_psa_driver_ghash.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_psa_driver_init.c
#  ${SECURITY_DIR}/sl_psa_driver/src/sli_psa_trng.c
)
zephyr_library_link_libraries(mbedTLSBase)

endif() # Shared files for all drivers

# HSE driver
zephyr_library_sources_ifdef(CONFIG_PSA_CRYPTO_DRIVER_SILABS_HSE
  ${SECURITY_DIR}/se_manager/src/sl_se_manager_cipher.c
  ${SECURITY_DIR}/se_manager/src/sl_se_manager_hash.c
  ${SECURITY_DIR}/se_manager/src/sl_se_manager_key_derivation.c
  ${SECURITY_DIR}/se_manager/src/sl_se_manager_key_handling.c
  ${SECURITY_DIR}/se_manager/src/sl_se_manager_signature.c
  ${SECURITY_DIR}/se_manager/src/sl_se_manager_attestation.c

  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_driver_aead.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_driver_builtin_keys.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_driver_cipher.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_driver_key_derivation.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_driver_key_management.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_driver_mac.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_driver_signature.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_opaque_driver_aead.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_opaque_driver_cipher.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_opaque_driver_mac.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_opaque_key_derivation.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_transparent_driver_aead.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_transparent_driver_cipher.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_transparent_driver_hash.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_transparent_driver_mac.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_transparent_key_derivation.c
  ${SECURITY_DIR}/sl_psa_driver/src/sli_se_version_dependencies.c
)

endif() # CONFIG_MBEDTLS_PSA_CRYPTO_SILABS_DRIVER_WRAPPER