#define CONFIG_PAGE "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n\
<html>\
<head></head>\
<meta name='viewport' content='width=device-width, initial-scale=1'>\
<body>\
<h1>ESP WiFi NAT Router Config</h1>\
<div id='config'>\
<script>\
if (window.location.search.substr(1) != '')\
{\
document.getElementById('config').display = 'none';\
document.body.innerHTML ='<h1>ESP WiFi NAT Router Config</h1>The new settings have been sent to the device...';\
setTimeout(\"location.href = '/'\",10000);\
}\
</script>\
<h2>STA Settings</h2>\
<form action='' method='GET'>\
<table>\
<tr>\
<td>SSID:</td>\
<td><input type='text' name='ssid' value='%s'/></td>\
</tr>\
<tr>\
<td>Password:</td>\
<td><input type='password' name='password' value='%s'/></td>\
</tr>\
<td>Automesh:</td>\
<td><input type='checkbox' name='am' value='mesh' %s></td>\
</tr>\
<tr>\
<td></td>\
<td><input type='submit' value='Connect'/></td>\
</tr>\
\
</table>\
</form>\
\
<h2>AP Settings</h2>\
<form action='' method='GET'>\
<table>\
<tr>\
<td>SSID:</td>\
<td><input type='text' name='ap_ssid' value='%s'/></td>\
</tr>\
<tr>\
<td>Password:</td>\
<td><input type='text' name='ap_password' value='%s'/></td>\
</tr>\
<tr>\
<td>Security:</td>\
<td>\
 <select name='ap_open'>\
 <option value='open'%s>Open</option>\
 <option value='wpa2'%s>WPA2</option>\
</select>\
</td>\
</tr>\
<tr>\
<td>Subnet:</td>\
<td><input type='text' name='network' value='%d.%d.%d.%d'/></td>\
</tr>\
<tr>\
<td></td>\
<td><input type='submit' value='Set' /></td>\
</tr>\
</table>\
<small>\
<i>Password: </i>min. 8 chars<br />\
</small>\
</form>\
\
<h2>Lock Config</h2>\
<form action='' method='GET'>\
<table>\
<tr>\
<td>Lock Device:</td>\
<td><input type='checkbox' name='lock' value='l'></td>\
</tr>\
<tr>\
<td></td>\
<td><input type='submit' name='dolock' value='Lock'/></td>\
</tr>\
</table>\
</form>\
\
<h2>Device Management</h2>\
<form action='' method='GET'>\
<table>\
<tr>\
<td>Reset Device:</td>\
<td><input type='submit' name='reset' value='Restart'/></td>\
</tr>\
</table>\
</form>\
</div>\
</body>\
</html>\
"

#define LOCK_PAGE "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n\
<html>\
<head></head>\
<meta name='viewport' content='width=device-width, initial-scale=1'>\
<body>\
<h1>ESP WiFi NAT Router Config</h1>\
<div id='config'>\
<script>\
if (window.location.search.substr(1) != '')\
{\
document.getElementById('config').display = 'none';\
document.body.innerHTML ='<h1>ESP WiFi NAT Router Config</h1>Unlock request has been sent to the device...';\
setTimeout(\"location.href = '/'\",1000);\
}\
</script>\
<h2>Config Locked</h2>\
<form autocomplete='off' action='' method='GET'>\
<table>\
<tr>\
<td>Password:</td>\
<td><input type='password' name='unlock_password'/></td>\
</tr>\
<tr>\
<td></td>\
<td><input type='submit' value='Unlock'/></td>\
</tr>\
\
</table>\
<small>\
<i>Default: STA password to unlock<br />\
</small>\
</form>\
</div>\
</body>\
</html>\
"
