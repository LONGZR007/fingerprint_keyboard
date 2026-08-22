编译方法：
解压编译工具：
sdk/MRS_Toolchain_Linux_X64_V240.tar.xz
设置编译器路径，路径需要根据实际情况进行调整：
export PATH="sdk/MRS_Toolchain_Linux_X64_V240/Toolchain/RISC-V Embedded GCC12/bin:$PATH"
编译：
make -j16 all
清理编译：
make -j16S clean
