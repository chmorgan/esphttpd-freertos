<html>
<head>
  <title>WiFi connection</title>
  <link rel="stylesheet" type="text/css" href="./style.css">
  <script type="text/javascript" src="../static/140medley.min.js"></script>
  <script type="text/javascript">

    var xhr = j();
    var currAp = "%currSsid%";
    var api_base = "/wifi/"

    function createInputForAp(ap) {
      if (ap.essid == "" && ap.rssi == 0) return;
      var div = document.createElement("div");
      div.id = "apdiv";
      var rssi = document.createElement("div");
      var rssiVal = -Math.floor(ap.rssi / 51) * 32;
      rssi.className = "icon";
      rssi.style.backgroundPosition = "0px " + rssiVal + "px";
      var encrypt = document.createElement("div");
      var encVal = "-64"; //assume wpa/wpa2
      if (ap.enc == "0") encVal = "0"; //open
      if (ap.enc == "1") encVal = "-32"; //wep
      encrypt.className = "icon";
      encrypt.style.backgroundPosition = "-32px " + encVal + "px";
      var input = document.createElement("input");
      input.type = "radio";
      input.name = "radio-essid";
      input.value = ap.essid;
      if (currAp != "" && currAp == ap.essid) input.checked = "1";
      input.id = "opt-" + ap.essid;
      input.addEventListener("change", () => { document.getElementById("txt_essid").value = ap.essid })
      var label = document.createElement("label");
      label.htmlFor = "opt-" + ap.essid;
      label.textContent = ap.essid;
      div.appendChild(input);
      div.appendChild(rssi);
      div.appendChild(encrypt);
      div.appendChild(label);
      return div;
    }

    function getSelectedEssid() {
      var e = document.forms.wifiform.elements;
      for (var i = 0; i < e.length; i++) {
        if (e[i].type == "radio" && e[i].checked) return e[i].value;
      }
      return currAp;
    }


    function scanAPs() {
      xhr.open("GET", api_base + "wifiscan.cgi");
      xhr.onreadystatechange = function () {
        if (xhr.readyState == 4 && xhr.status >= 200 && xhr.status < 300) {
          var data = JSON.parse(xhr.responseText);
          currAp = getSelectedEssid();
          if (data.result.inProgress == "0" && data.result.APs.length > 1) {
            $("#aps").innerHTML = "";
            for (var i = 0; i < data.result.APs.length; i++) {
              if (data.result.APs[i].essid == "" && data.result.APs[i].rssi == 0) continue;
              $("#aps").appendChild(createInputForAp(data.result.APs[i]));
            }
            //window.setTimeout(scanAPs, 20000);
          } else if (data.result.inProgress == "1") {
            window.setTimeout(scanAPs, 1000);
            $("#aps").innerHTML = "Scanning APs...";
          } else { // some error..
            $("#aps").innerHTML = "AP scan error!";
          }
        }
      }
      xhr.send();
    }


    window.onload = function (e) {
      //scanAPs();
    };
  </script>
</head>
<body>
  <button onclick="location.href='/'">Back</button>
  <div id="main">
    <div style="margin-top: 30px; padding: 0px 10px; border: 1px solid;">
      <h4>WiFi Mode</h4>
      <p>
        Current WiFi mode: <b> %WiFiMode% </b>
      </p>
      <p>
        %ModeWarn%
      </p>
    </div>
    <div style="margin-top: 30px; padding: 0px 10px; border: 1px solid;">
      <h4>WiFi Client (STAtion Mode)</h4>
      <form name="wifiform" action="/wifi/connect.cgi" method="post">
        <p>
          %StaWarn%
        </p>
        <p>
          To connect to a WiFi network, please select one of the detected networks...<br>
          <button id="scan-btn" type="button" onclick="scanAPs()">Scan!</button> Warning: scanning may interrupt
          ongoing communication.
          <div id="aps">Click Scan to discover WiFi APs!...</div>
          <br>
          Selected SSID: <br>
          <input type="text" name="essid" value="%currSsid%" id="txt_essid"> <br>
          WiFi password, if applicable: <br>
          <input type="password" name="passwd" value="%WiFiPasswd%" id="txt_passwd"> <br>
          <input type="submit" name="connect" value="Connect!">
        </p>
      </form>
    </div>
    <div style="margin-top: 30px; padding: 0px 10px; border: 1px solid;">
      <h4>WiFi Access Point (AP Mode)</h4>
      <form name="apform" action="/wifi/ap" method="post">
        <p>
          %ApWarn%
        </p>
        <p>
          AP SSID: <br>
          <input type="text" name="ssid" value="%ApSsid%" pattern=".{1,32}" required title="1 to 32 characters"> <br>
          AP password, if applicable: <br>
          <input type="password" name="pass" value="%ApPass%" pattern=".{8,64}" required title="8 to 64 characters">
          <br>

          AP Channel: <br>
          <input type="number" name="chan" value="%ApChan%" min="1" max="14" required title="Integer 1~14 required">
          <br>
          <input type="submit" name="save" value="Save AP Settings">
        </p>
      </form>
    </div>
  </div>
</body>

</html>