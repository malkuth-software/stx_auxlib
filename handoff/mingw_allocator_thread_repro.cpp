#include "stx_auxlib/math.hpp"
#include <cstdio>
#include <thread>
#include <vector>
static float r[4];
void work(int i){ float acc=0; for(int k=0;k<1000000;++k) acc+=stx_auxlib::math::exp((float)k*1e-6f); r[i]=acc; }
int main(){ std::printf("start\n"); std::fflush(stdout);
 float one = stx_auxlib::math::exp(1.0f); std::printf("one %g\n", one); std::fflush(stdout);
 std::vector<std::thread> p; for(int i=0;i<4;++i) p.emplace_back(work,i); for(auto&t:p) t.join(); std::printf("%g\n", r[0]); }
