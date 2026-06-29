// large dense switch (128 cases) → jump table; stresses indirect-branch table codegen.
#include <stdio.h>
static long dispatch(int k){
  switch(k){
#define C(n) case n: return (long)n*n - 3*n + 7;
    C(0)C(1)C(2)C(3)C(4)C(5)C(6)C(7)C(8)C(9)C(10)C(11)C(12)C(13)C(14)C(15)
    C(16)C(17)C(18)C(19)C(20)C(21)C(22)C(23)C(24)C(25)C(26)C(27)C(28)C(29)C(30)C(31)
    C(32)C(33)C(34)C(35)C(36)C(37)C(38)C(39)C(40)C(41)C(42)C(43)C(44)C(45)C(46)C(47)
    C(48)C(49)C(50)C(51)C(52)C(53)C(54)C(55)C(56)C(57)C(58)C(59)C(60)C(61)C(62)C(63)
    C(64)C(65)C(66)C(67)C(68)C(69)C(70)C(71)C(72)C(73)C(74)C(75)C(76)C(77)C(78)C(79)
    C(80)C(81)C(82)C(83)C(84)C(85)C(86)C(87)C(88)C(89)C(90)C(91)C(92)C(93)C(94)C(95)
    C(96)C(97)C(98)C(99)C(100)C(101)C(102)C(103)C(104)C(105)C(106)C(107)C(108)C(109)
    C(110)C(111)C(112)C(113)C(114)C(115)C(116)C(117)C(118)C(119)C(120)C(121)C(122)
    C(123)C(124)C(125)C(126)C(127)
#undef C
    default: return -1;
  }
}
int main(void){ long s=0; for(int i=0;i<100000;i++) s+=dispatch(i&127); printf("s=%ld\n",s); return 0; }
