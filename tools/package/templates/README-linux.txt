Ember @VERSION@ — Linux x86-64（无 AVX2 要求）
=================================================

这个包里的二进制按 x86-64 基线编译（SSE2），整包反汇编里 0 条 ymm/zmm 指令，
所以不支持 AVX2 的老机器在指令集这一层没问题。

跑起来需要两样东西：
  1) libcurl.so.4            （发行版里的 libcurl4 / curl 运行时包）
  2) glibc >= 2.30           （见下面"为什么是 2.30"）
参照：Debian 10+、Ubuntu 19.10+、Fedora 31+、Arch 都行；CentOS 7/8、Debian 9 跑不了。

装好之后（务必先离开 U 盘）：
  mkdir -p ~/ember && cp ember-@VERSION@-linux-x86_64.tar.gz ~/ember/
  cd ~/ember && tar xzf ember-@VERSION@-linux-x86_64.tar.gz
  cd ember-@VERSION@-linux-x86_64
  chmod +x ember verify.sh test_*        # U 盘（FAT32）不带执行位，拷到硬盘再 chmod
  ./ember --version
  sh verify.sh                           # 一屏告诉你这台机器上行不行（不联网、不要 API key）

verify.sh 会打印：glibc / 是否有 avx2 / 找不找得到 libcurl.so.4 / 冒烟 --help /
20 个离线测试的通过情况。把它的输出整屏贴回来就能判断要不要往下修。

为什么是 2.30（而不是上一版的 2.29）：
  app.cpp:770 那个"等用户回答"的 5 分钟超时用了 std::condition_variable，
  libc++ 在 glibc>=2.30 的目标上会引用 pthread_cond_clockwait@GLIBC_2.30。
  本包另外两个高版本符号：pow@GLIBC_2.29、copy_file_range@GLIBC_2.27。
  如果你的机器低于 2.30，两条修法：给这三个符号补兼容壳，或把 http 层改成调用
  系统 curl 程序，从而出一个零依赖静态 ELF。

Windows 版是同版本号的另一个包（Ember-@VERSION@-win64.zip），配置格式通用：
  settings.json 放当前目录即可（providers / groups / model 一套）。
