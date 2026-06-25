// t2csri_rubyeval_ext.cs - Extended rubyEval handlers (RSA, decrypt, etc)
function rubyEval(%code)
{
   // $accountKey.e/n/d = '...'
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

   // $accountKey.decrypt(...)
   if (strstr(%code, "decrypt") != -1)
   {
      %s = strstr(%code, "decrypt('") + 9;
      $temp = t2csri_native_rsaModExp(getSubStr(%code, %s, strLen(%code) - %s - 2),
         $t2csri::native::accountKeyD, $t2csri::native::accountKeyN);
      return;
   }

   // t2csri_verify_auth_signature(...)
   if (strstr(%code, "verify_auth_signature") != -1)
   {
      %openParenQuote = "('";
      %s = strstr(%code, %openParenQuote) + 2;
      $temp = sha1sum(getSubStr(%code, %s, strLen(%code) - %s - 2));
      return;
   }

   // Fallback to core handler
   rubyEvalCore(%code);
}
