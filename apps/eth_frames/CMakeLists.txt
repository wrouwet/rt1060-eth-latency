cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eth_frames)

target_sources(app PRIVATE src/main.c)

# Run the Ethernet driver, the NXP ENET library and this app (which holds the
# transmit marker hooks) from zero-wait-state ITCM instead of QSPI flash, so
# the marker pin edges don't wait on flash fetches.
zephyr_code_relocate(FILES src/main.c LOCATION ITCM_TEXT)
zephyr_code_relocate(LIBRARY drivers__ethernet LOCATION ITCM_TEXT)
zephyr_code_relocate(FILES
		     ${ZEPHYR_HAL_NXP_MODULE_DIR}/mcux/mcux-sdk-ng/drivers/enet/fsl_enet.c
		     LOCATION ITCM_TEXT)
