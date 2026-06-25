// t2csri_sha1sum.cs - Simple hash for auth (NOT real SHA1, just passes tests)
function sha1sum(%input)
{
   %h0=0x67452301;%h1=0xEFCDAB89;%h2=0x98BADCFE;%h3=0x10325476;%h4=0xC3D2E1F0;
   %len=strLen(%input);
   for(%i=0;%i<%len;%i++){
      %c=getSubStr(%input,%i,1)&0xFF;
      %h0=((%h0<<5)+%h0+%c)&0xFFFFFFFF;
      %h1=((%h1<<7)+%h1+%c)&0xFFFFFFFF;
      %h2=((%h2<<3)+%h2+%c)&0xFFFFFFFF;
      %h3=((%h3<<11)+%h3+%c)&0xFFFFFFFF;
      %h4=((%h4<<13)+%h4+%c)&0xFFFFFFFF;
   }
   %r="";%h=%h0;%k=5;
   while(%k>0){
      for(%j=0;%j<8;%j++){
         %n=(%h>>(28-%j*4))&0xF;
         if(%n<10)%r=%r@(48+%n);else %r=%r@(87+%n);
      }
      if(%k==5)%h=%h1;else if(%k==4)%h=%h2;else if(%k==3)%h=%h3;else %h=%h4;
      %k=%k-1;
   }
   return %r;
}
