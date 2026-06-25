// t2csri_rubyeval_core.ts - Core rubyEval (SHA1, tsEval, RSA init)
function rubyEvalCore(%code)
{
   if (strstr(%code, "tsEval") != -1)
   {
      %eq = strstr(%code, "= '");
      if (%eq == -1) return;
      %startIdx = %eq + 3;
      $temp = getSubStr(%code, %startIdx, strLen(%code) - %startIdx - 1);
      return;
   }
   if (strstr(%code, "SHA1Pure") != -1) return;
   if (strstr(%code, "prepare") != -1) return;
   if (strstr(%code, "append('") != -1)
   {
      %s = strstr(%code, "append('") + 8;
      $t2csri::native::sha1_input = $t2csri::native::sha1_input @ getSubStr(%code, %s, strLen(%code) - %s - 2);
      return;
   }
   if (strstr(%code, "hexdigest") != -1)
   {
      $temp = sha1sum($t2csri::native::sha1_input);
      $t2csri::native::sha1_input = "";
      return;
   }
   if (strstr(%code, "RSAKey") != -1)
   {
      $t2csri::native::accountKeyE = "0";
      $t2csri::native::accountKeyN = "0";
      $t2csri::native::accountKeyD = "0";
      return;
   }
   if (strstr(%code, "Time") != -1) { $temp = getUTC(); return; }
   if (strstr(%code, "rand(") != -1) { $temp = getRandom(1000); return; }
   if (strstr(%code, "certstore_") != -1) return;
}
