//
// SuperscopeExamples.h — the 14 classic Superscope example scripts, verbatim
// from AVS (e_superscope.h, BSD-3, Copyright 2005 Nullsoft, Inc.). They serve
// as bundled content for our scripted Superscope effect and as the validation
// corpus for the EEL VM.
//
#pragma once

namespace viz {

struct SuperscopeExample {
    const char* name;
    const char* init;
    const char* frame;
    const char* beat;
    const char* point;
};

inline constexpr SuperscopeExample kSuperscopeExamples[] = {
    { "Spiral",
      "n=800",
      "t=t-0.05",
      "",
      "d=i+v*0.2; r=t+i*$PI*4; x=cos(r)*d; y=sin(r)*d" },
    { "3D Scope Dish",
      "n=200",
      "",
      "",
      "iz=1.3+sin(r+i*$PI*2)*(v+0.5)*0.88; ix=cos(r+i*$PI*2)*(v+0.5)*.88;"
      " iy=-0.3+abs(cos(v*$PI)); x=ix/iz;y=iy/iz;" },
    { "Rotating Bow Thing",
      "n=80;t=0.0;",
      "t=t+0.01",
      "",
      "r=i*$PI*2; d=sin(r*3)+v*0.5; x=cos(t+r)*d; y=sin(t-r)*d" },
    { "Vertical Bouncing Scope",
      "n=100; t=0; tv=0.1;dt=1;",
      "t=t*0.9+tv*0.1",
      "tv=((rand(50.0)/50.0))*dt; dt=-dt;",
      "x=t+v*pow(sin(i*$PI),2); y=i*2-1.0;" },
    { "Spiral Graph Fun",
      "n=100;t=0;",
      "t=t+0.01;",
      "n=80+rand(120.0)",
      "r=i*$PI*128+t; x=cos(r/64)*0.7+sin(r)*0.3; y=sin(r/64)*0.7+cos(r)*0.3" },
    { "Alternating Diagonal Scope",
      "n=64; t=1;",
      "",
      "t=-t;",
      "sc=0.4*sin(i*$PI); x=2*(i-0.5-v*sc)*t; y=2*(i-0.5+v*sc);" },
    { "Vibrating Worm",
      "n=w; dt=0.01; t=0; sc=1;",
      "t=t+dt;dt=0.9*dt+0.001; t=if(above(t,$PI*2),t-$PI*2,t);",
      "dt=sc;sc=-sc;",
      "x=cos(2*i+t)*0.9*(v*0.5+0.5); y=sin(i*2+t)*0.9*(v*0.5+0.5);" },
    { "Wandering Simple",
      "n=800;xa=-0.5;ya=0.0;xb=-0.0;yb=0.75;c=200;f=0;\n"
      "nxa=(rand(100)-50)*.02;nya=(rand(100)-50)*.02;\n"
      "nxb=(rand(100)-50)*.02;nyb=(rand(100)-50)*.02;",
      "f=f+1;\n"
      "t=1-((cos((f*3.1415)/c)+1)*.5);\n"
      "xa=((nxa-lxa)*t)+lxa;\n"
      "ya=((nya-lya)*t)+lya;\n"
      "xb=((nxb-lxb)*t)+lxb;\n"
      "yb=((nyb-lyb)*t)+lyb;\n"
      "ex=(xb-xa);\n"
      "ey=(yb-ya);\n"
      "d=sqrt(sqr(ex)+sqr(ey));\n"
      "r=atan(ey/ex)+(3.1415/2);\n"
      "dv=d*2",
      "c=f;\n"
      "f=0;\n"
      "lxa=nxa;\n"
      "lya=nya;\n"
      "lxb=nxb;\n"
      "lyb=nyb;\n"
      "nxa=(rand(100)-50)*.02;\n"
      "nya=(rand(100)-50)*.02;\n"
      "nxb=(rand(100)-50)*.02;\n"
      "nyb=(rand(100)-50)*.02",
      "//primary render\n"
      "x=(ex*i)+xa;\n"
      "y=(ey*i)+ya;\n"
      "\n"
      "//volume offset\n"
      "x=x+ ( cos(r) * v * dv);\n"
      "y=y+ ( sin(r) * v * dv);\n"
      "\n"
      "//color values\n"
      "red=i;\n"
      "green=(1-i);\n"
      "blue=abs(v*6);" },
    { "Flitterbug",
      "n=180;t=0.0;lx=0;ly=0;vx=rand(200)-100;vy=rand(200)-100;cf=.97;c=200;f=0",
      "x=nx;y=ny;\n"
      "r=i*3.14159*2; f=f+1;t=(f*2*3.1415)/c;\n"
      "vx=(vx-(lx*.1))*cf;\n"
      "vy=(vy-(ly*.1))*cf;\n"
      "lx=lx+vx;ly=ly+vy;\n"
      "nx=lx*.001;ny=ly*.001;\n"
      "s=abs(nx*ny)",
      "c=f;f=0;\n"
      "vx=vx+rand(600)-300;vy=vy+rand(600)-300",
      "d=(sin(r*5*(1-s))+i*0.5)*(.3-s);\n"
      "tx=(t*(1-(s*(i-.5))));\n"
      "x=x+cos(tx+r)*d; y=y+sin(t-y)*d;\n"
      "red=abs(x-nx)*5;\n"
      "green=abs(y-ny)*5;\n"
      "blue=1-s-red-green;" },
    { "Spirostar",
      "n=20;t=0;f=0;c=200;mn=10;dv=2;dn=0",
      "f=f+1;t=(f*3.1415*2)/c;\n"
      "sz=abs(sin(t-3.1415));\n"
      "dv=if(below(n,12),(n/2)-1,\n"
      "    if(equal(12,n),3,\n"
      "    if(equal(14,n),6,\n"
      "    if(below(n,20),2,4))))",
      "bb = bb + 1;\n"
      "beatdiv = 8;\n"
      "c=if(equal(bb%beatdiv,0),f,c);\n"
      "f=if(equal(bb%beatdiv,0),0,f);\n"
      "g=if(equal(bb%beatdiv,0),g+1,g);\n"
      "n=if(equal(bb%beatdiv,0),(abs((g%17)-8) *2)+4,n);",
      "r=if(b,0,((i*dv)*3.14159*128)+(t/2));\n"
      "x=cos(r)*sz;\n"
      "y=sin(r)*sz;" },
    { "Exploding Daisy",
      "n = 380 + rand(200) ; k = 0.0; l = 0.0; m = ( rand( 10 ) + 2 ) * .5;"
      " c = 0; f = 0",
      "a = a + 0.002 ; k = k + 0.04 ; l = l + 0.03",
      "bb = bb + 1;\n"
      "beatdiv = 16;\n"
      "n=if(equal(bb%beatdiv,0),380 + rand(200),n);\n"
      "t=if(equal(bb%beatdiv,0),0.0,t);\n"
      "a=if(equal(bb%beatdiv,0),0.0,a);\n"
      "k=if(equal(bb%beatdiv,0),0.0,k);\n"
      "l=if(equal(bb%beatdiv,0),0.0,l);\n"
      "m=if(equal(bb%beatdiv,0),(( rand( 100  ) + 2 ) * .1) + 2,m);",
      "r=(i*3.14159*2)+(a * 3.1415);\n"
      "d=sin(r*m)*.3;\n"
      "x=cos(k+r)*d*2;y=(  (sin(k-r)*d) + ( sin(l*(i-.5) ) ) ) * .7;\n"
      "red=abs(x);\n"
      "green=abs(y);\n"
      "blue=d" },
    { "Swirlie Dots",
      "n=45;t=rand(100);u=rand(100)",
      "t = t + .15; u = u + .05",
      "bb = bb + 1;\n"
      "beatdiv = 16;\n"
      "n = if(equal(bb%beatdiv,0),30 + rand( 30 ),n);",
      "di = ( i - .5) * 2;\n"
      "x = di;sin(u*di) * .4;\n"
      "y = cos(u*di) * .6;\n"
      "x = x + ( cos(t) * .05 );\n"
      "y = y + ( sin(t) * .05 );" },
    { "Sweep",
      "n=180;lsv=100;sv=200;ssv=200;c=200;f=0",
      "f=f+1;t=(f*2*3.1415)/c;\n"
      "lsv=slsv;sv=ssv;fv=0",
      "bb = bb + 1;\n"
      "beatdiv = 8;\n"
      "c=if(equal(bb%beatdiv,0),f,c);\n"
      "f=if(equal(bb%beatdiv,0),0,f);\n"
      "dv=if(equal(bb%beatdiv,0),((rand(100)*.01) * .1) + .02,dv);\n"
      "n=if(equal(bb%beatdiv,0),80+rand(100),n);\n"
      "ssv=if(equal(bb%beatdiv,0),rand(200)+100,ssv);\n"
      "slsv=if(equal(bb%beatdiv,0),rand(200)+100,slsv);",
      "sv=(sv*abs(cos(lsv)))+(lsv*abs(cos(sv)));\n"
      "fv=fv+(sin(sv)*dv);\n"
      "d=i; r=t+(fv * sin(t) * .3)*3.14159*4;\n"
      "x=cos(r)*d;\n"
      "y=sin(r)*d;\n"
      "red=i;\n"
      "green=abs(sin(r))-(red*.15);\n"
      "blue=fv" },
    { "Whiplash Spiral",
      "n=80;c=200;f=0",
      "t=t-0.05;f=f+1;dt=(f*2*3.1415)/c",
      "bb = bb + 1;\n"
      "beatdiv = 8;\n"
      "c=if(equal(bb%beatdiv,0),f,c);\n"
      "f=if(equal(bb%beatdiv,0),0,f);",
      "d=i;\n"
      "r=t+i*3.14159*4;\n"
      "sdt=sin(dt+(i*3.1415*2));\n"
      "cdt=cos(dt+(i*3.1415*2));\n"
      "x=(cos(r)*d) + (sdt * .6 * sin(t) );\n"
      "y=(sin(r)*d) + ( cdt *.6 * sin(t) );\n"
      "blue=abs(x);\n"
      "green=abs(y);\n"
      "red=cos(dt*4)" },
};

inline constexpr int kNumSuperscopeExamples =
    (int)(sizeof(kSuperscopeExamples) / sizeof(kSuperscopeExamples[0]));

} // namespace viz
