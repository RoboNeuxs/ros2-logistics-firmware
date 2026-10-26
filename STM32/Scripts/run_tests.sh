#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p Build
gcc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DMOTOR_HOST_TEST -ITests/mocks -IApplication -IBoard \
    Tests/test_firmware.c Application/*.c -o Build/test_firmware
./Build/test_firmware
gcc -std=c99 -Wall -Wextra -Werror -fsanitize=undefined \
    -DMOTOR_HOST_TEST -DMOTOR_HAL_TEST -ITests/hardware_mocks \
    -IApplication -IBoard -IDrivers/CMSIS/Device/Include \
    Tests/test_stm32_hal.c Board/motor_hal.c Application/*.c -o Build/test_stm32_hal
./Build/test_stm32_hal
gcc -std=c99 -Wall -Wextra -Werror -shared -fPIC -fsanitize=undefined \
    -DMOTOR_INTEGRATION_TEST -DMOTOR_HOST_TEST -ITests/mocks -IApplication -IBoard \
    Tests/test_firmware.c Application/*.c -o Build/firmware_test.so
MOTOR_FIRMWARE_LIB="$(pwd)/Build/firmware_test.so" python3 -m unittest discover -s Tests -p test_integration.py -v
python3 -m unittest discover -s Tests -p test_host.py -v
