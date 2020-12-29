<p>#define CONFIG_PAGE "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n\\</p>
<p>\\</p>
<p>\</p>
<h1 style="text-align: center;"><span style="background-color: #339966; color: #ffffff;"><em>&nbsp;SmartHomes</em>&nbsp;</span></h1>
<p>\</p>
<h2 style="text-align: center;"><span style="text-decoration: underline; color: #ffffff;"><span style="background-color: #99cc00;"><strong>&nbsp;Smart you,Smart choice</strong></span></span><span style="text-decoration: underline; color: #ffffff;"><span style="background-color: #99cc00;"><strong>&nbsp;</strong></span></span></h2>
<p>\</p>
<div id="config">\ \
<h2>&nbsp;<em>&nbsp;&nbsp;<span style="background-color: #ff9900; color: #ffffff;">&nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; Set WIFI&nbsp; &nbsp; &nbsp; &nbsp; &nbsp;</span></em></h2>
\<form action="" method="GET">\\\\\\\\\\Automesh:\<input name="am" type="checkbox" value="mesh" />\ \\\\\ \
<table style="width: 315px;">
<tbody>
<tr>
<td style="width: 107px;"><strong>SSID:</strong></td>
<td style="width: 192px;"><input name="ssid" type="text" value="%s" /></td>
</tr>
<tr>
<td style="width: 107px;"><strong>Password:</strong></td>
<td style="width: 192px;"><input name="password" type="text" value="%s" /></td>
</tr>
<tr>
<td style="width: 107px;">&nbsp;</td>
<td style="width: 192px;"><input type="submit" value="Connect" /></td>
</tr>
</tbody>
</table>
\</form>\ \
<h2><span style="color: #ffffff; background-color: #ff9900;">&nbsp; &nbsp; &nbsp; <strong><em>&nbsp;Set HOTSPOT&nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp;&nbsp;</em></strong></span></h2>
\<form action="" method="GET">\\\\\\\\\\\\\\\\\\\\\\
<table>
<tbody>
<tr>
<td><span style="color: #ffffff; background-color: #ff6600;"><strong><em>SSID:&nbsp; &nbsp; &nbsp; &nbsp;&nbsp;</em></strong></span></td>
<td><input name="ap_ssid" type="text" value="%s" /></td>
</tr>
<tr>
<td><span style="color: #ffffff; background-color: #ff6600;"><strong><em>Password:</em></strong></span></td>
<td><input name="ap_password" type="text" value="%s" /></td>
</tr>
<tr>
<td><span style="color: #ffffff; background-color: #ff6600;"><strong><em>Security:&nbsp; &nbsp;</em></strong></span></td>
<td>\<select name="ap_open">\
<option value="open">Open</option>
\
<option value="wpa2">WPA2</option>
\</select>\</td>
</tr>
<tr>
<td><span style="background-color: #ff6600; color: #ffffff;"><strong><em>Subnet:&nbsp; &nbsp; &nbsp;</em></strong></span></td>
<td><input name="network" type="text" value="%d.%d.%d.%d" /></td>
</tr>
<tr>
<td>&nbsp;</td>
<td><input type="submit" value="Set" /></td>
</tr>
</tbody>
</table>
\ <small>\ <em>Password: </em>min. 8 chars<br />\ </small>\</form>\ \
<h2>&nbsp; &nbsp; &nbsp; <span style="color: #ffffff; background-color: #ff0000;">&nbsp;<strong><em> &nbsp;Lock Config</em></strong>&nbsp; &nbsp;&nbsp;</span></h2>
\<form action="" method="GET">\\\\\\\\\\
<table style="width: 165px;">
<tbody>
<tr>
<td style="width: 108px;">
<p><span style="background-color: #33cccc; color: #ffffff;"><em><strong>&nbsp; &nbsp; &nbsp; &nbsp; Lock&nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp; &nbsp;</strong></em></span><span style="background-color: #33cccc; color: #ffffff;"><em><strong>Device:&nbsp;&nbsp;</strong></em></span></p>
</td>
<td style="width: 41px;"><input name="lock" type="checkbox" value="l" /></td>
</tr>
<tr>
<td style="width: 108px;">&nbsp;</td>
<td style="width: 41px;"><input name="dolock" type="submit" value="Lock" /></td>
</tr>
</tbody>
</table>
\</form>\ \
<h2><span style="color: #ffffff; background-color: #ff0000;"><strong><em>&nbsp;Device Management&nbsp;&nbsp;</em></strong></span></h2>
\<form action="" method="GET">\\\\\\
<table style="width: 188px;">
<tbody>
<tr style="height: 39.7188px;">
<td style="width: 98px; height: 39.7188px; text-align: left;"><span style="color: #ffffff;"><em><strong><span style="background-color: #33cccc;">&nbsp; &nbsp; Reset&nbsp; &nbsp; Device :&nbsp; &nbsp; &nbsp; &nbsp;</span></strong></em></span></td>
<td style="width: 74px; height: 39.7188px;"><span style="background-color: #33cccc;"><input name="reset" type="submit" value="Restart" /></span></td>
</tr>
</tbody>
</table>
\</form>\</div>
<p>\</p>
<p>\\ " #define LOCK_PAGE "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n\\</p>
<p>\\</p>
<p>\</p>
<h1>&nbsp;</h1>
<p>\</p>
<div id="config">\ \
<h2>&nbsp;</h2>
\<form action="" autocomplete="off" method="GET">\\\\\\\\\\ \\ <small>\ <em><br />\ </em></small>\</form>\</div>
<p>\</p>
<p>\\ "</p>
