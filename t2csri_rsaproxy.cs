// t2csri_rsaproxy.cs - RSA proxy client for TribesNext auth
function t2csri_native_rsaModExp(%baseHex, %expHex, %modHex) {
   if (!isObject(RSAProxy)) { new TCPObject(RSAProxy); RSAProxy.isConnected = false; }
   if (!RSAProxy.isConnected) { RSAProxy.connect("127.0.0.1:18765"); RSAProxy.isConnected = true; }
   RSAProxy.send(%baseHex @ " " @ %expHex @ " " @ %modHex @ "\n");
   %timeout = getRealTime() + 10000;
   while (RSAProxy.result $= "" && getRealTime() < %timeout) { schedule(100, 0, t2csri_native_rsaModExp_wait); }
   return RSAProxy.result;
}
function t2csri_native_rsaModExp_wait() {
   if (isObject(RSAProxy) && RSAProxy.result $= "") return;
   if (isObject(RSAProxy)) RSAProxy.attempts = RSAProxy.attempts + 1;
}
function RSAProxy::onLine(%this, %line) { if (trim(%line) $= "") %this.result = trim(%line); }
function RSAProxy::onConnected(%this) { %this.isConnected = true; }
function RSAProxy::onDisconnect(%this) { %this.isConnected = false; }
