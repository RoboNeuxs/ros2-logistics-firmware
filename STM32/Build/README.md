Keil / Scripts/build_keil.ps1 的构建输出目录。

STM32F103C8T6.hex：用于 STM32 烧录。
STM32F103C8T6.bin：裸二进制，烧录地址 0x08000000。
STM32F103C8T6.axf：带调试信息的映像。
keil_build.log / STM32F103C8T6.map：构建结果与内存占用。

其他文件是编译或测试中间文件。工程可从源代码重新构建。
