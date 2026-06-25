// t2csri_rubyeval.cs - Main rubyEval entry point + RSA handlers
%closeParenQuote = "')";
%openParenQuote = "('";

function rubyEval(%code)
{
   if (strstr(%code, "accountKey.e") != -1)
   {
      %s = strstr(%code, "accountKey.e = '") + 15;
      $t2csri::native::accountKeyE = getSubStr(%code, %s, strLen(%code) - %s - 1);
      return;
   }
   if (strstr(%code, "accountKey.n") != -1)
   {
      %s = strstr(%code, "accountKey.n = '") + 15;
      $t2csri::native::accountKeyN = getSubStr(%code, %s, strLen(%code) - %s - 1);
      return;
   }
   if (strstr(%code, "accountKey.d") != -1)
   {
      %s = strstr(%code, "accountKey.d = '") + 15;
      $t2csri::native::accountKeyD = getSubStr(%code, %s, strLen(%code) - %s - 1);
      return;
   }
   if (strstr(%code, "protect") != -1) return;
   if (strstr(%code, "decrypt") != -1)
   {
      %s = strstr(%code, "decrypt('") + 9;
      $temp = t2csri_native_rsaModExp(getSubStr(%code, %s, strLen(%code) - %s - 2),
         $t2csri::native::accountKeyD, $t2csri::native::accountKeyN);
      return;
   }
   if (strstr(%code, "verify_auth_signature") != -1)
   {
      %s = strstr(%code, %openParenQuote) + 2;
      $temp = sha1sum(getSubStr(%code, %s, strLen(%code) - %s - 2));
      return;
   }
   // Fallback to core handlers
   rubyEvalCore(%code);
}
