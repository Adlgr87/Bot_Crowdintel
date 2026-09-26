# CMake generated Testfile for 
# Source directory: /home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core
# Build directory: /home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core/build_baseline
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(keccak_known_answer "/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core/build_baseline/bin/test_signer")
set_tests_properties(keccak_known_answer PROPERTIES  _BACKTRACE_TRIPLES "/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core/CMakeLists.txt;108;add_test;/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core/CMakeLists.txt;0;")
add_test(latency_benchmark "/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core/build_baseline/bin/latency_bench")
set_tests_properties(latency_benchmark PROPERTIES  _BACKTRACE_TRIPLES "/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core/CMakeLists.txt;111;add_test;/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/core/CMakeLists.txt;0;")
