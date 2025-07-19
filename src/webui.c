<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>OPENIPC AP-FPV</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    /* ------- palette & base -------------------------------------------- */
    :root { --blue:#007bff; --blue-dk:#0056b3; --bg:#eef4fb; --txt:#003366;
            --panel:#ffffff; --panel-brd:#0056b3; --slider-bg:#f5f9ff; }
    body  { margin:0; font-family:sans-serif; background:var(--bg);
            color:var(--txt); padding:1.5em; }
    h1    { text-align:center; color:#0059b3; font-size:1.6em; margin:.1em 0; }

    /* ------- tab buttons ------------------------------------------------ */
    .tabs { display:flex; max-width:340px; width:90%; margin:1em auto 0; }
    .tab-btn {
      flex:1; padding:.7em 0; font-size:1em; text-align:center;
      background:var(--bg); color:var(--blue); border:1px solid var(--blue);
      border-bottom:none; border-radius:6px 6px 0 0; cursor:pointer;
      user-select:none;
    }
    .tab-btn + .tab-btn { margin-left:.2em; }
    .tab-btn.active {
      background:#fff; color:var(--txt); border-color:var(--blue-dk);
    }

    /* ------- tab content ------------------------------------------------ */
    .tab-content {
      max-width:340px; width:90%; margin:0 auto 2em;
      background:var(--panel); border:1px solid var(--panel-brd);
      border-radius:0 6px 6px 6px; padding:1em;
    }
    .tab-content.hidden { display:none; }

    /* ------- row layout ------------------------------------------------- */
    .row  { display:flex; justify-content:center; align-items:center;
            margin:.4em 0; gap:.5em; }
    .btn  { flex:0 0 120px; width:120px; padding:.7em 0; font-size:1em;
            color:#fff; background:var(--blue); border:0; border-radius:6px;
            cursor:pointer; white-space:nowrap; }
    .btn:hover { background:var(--blue-dk); }

    .btn.info { background:#28a745; }
    .btn.info:hover { background:#218838; }

    select,input { flex:1; padding:.6em; font-size:1em; border:1px solid #ccc;
                   border-radius:6px; min-width:0; }

    .danger      { background:#dc3545; }
    .danger:hover{ background:#b02a37; }

    /* ------- cohesive slider block ------------------------------------- */
    .slider-container {
      flex-direction:column; align-items:center;
      width:100%;
      box-sizing:border-box;
      background:var(--slider-bg); border:1px solid var(--blue);
      border-radius:6px; padding:.7em .8em;
    }
    .slider-container label {
      font-size:1em; font-weight:600; width:100%; text-align:center;
      margin-bottom:.4em;
    }
    input[type=range] {
      -webkit-appearance:none;
      width:92%;
      height:8px; background:#ddd; border-radius:4px; cursor:pointer;
    }
    input[type=range]:focus { outline:none; }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance:none;
      width:24px; height:24px; margin-top:-8px;
      background:var(--blue); border-radius:50%; cursor:pointer;
      touch-action:none;
    }
    input[type=range]::-moz-range-thumb {
      width:24px; height:24px; background:var(--blue);
      border:none; border-radius:50%; cursor:pointer; touch-action:none;
    }

    /* ------- info box --------------------------------------------------- */
    .info-box { margin-top:1em; background:#dceaff; border:1px solid #339;
                border-radius:6px; padding:1em; min-height:6em;
                white-space:pre-wrap; font-size:.9em; }
  </style>
</head>
<body>

<h1>OPENIPC AP-FPV</h1>
<center><body>---<::: RC6 aalink4h :::>---</body></center>

<!-- Tabs -->
<div class="tabs">
  <div class="tab-btn active" id="tabControl">Control</div>
  <div class="tab-btn"        id="tabSetup">Setup</div>
</div>

<!-- Control Tab Content -->
<div class="tab-content" id="contentControl">

  <!-- Exec VTX cmd -->
  <div class="row">
    <button class="btn" onclick="goArg('control_cmd', sendVTXCmd.value)">VTX CMD</button>
    <select id="sendVTXCmd">
      <option value="vtx1">VTX cmd1</option>
      <option value="vtx2">VTX cmd2</option>
      <option value="vtx2">VTX cmd3</option>
    </select>
  </div>
  
  <!-- Exec VRX master cmd -->
  <div class="row">
    <button class="btn" onclick="goArg('control_cmd', sendVRXCmd.value)">VRX CMD</button>
    <select id="sendVRXCmd">
      <option value="vrx_ping">Ping VRX</option>
      <option value="vrx_toggle_rec">Toggle VRX record</option>
      <option value="vrx_shutdown">Shutdown VRX</option>
    </select>
  </div>

  <!-- aalink control -->
  <div class="row">
    <button class="btn" onclick="goArg('control_cmd', sendAalinkCmd.value)">aalink CMD</button>
    <select id="sendAalinkCmd">
      <option value="aalink_signalbar_enable">Signalbar enable</option>
      <option value="aalink_signalbar_disable">Signalbar disable</option>
      <option value="aalink_osd_level 0">No OSD</option>
      <option value="aalink_osd_level 1">Compact OSD</option>
      <option value="aalink_osd_level 2">Full OSD</option>
      <option value="aalink_font_size 30">Small font</option>
      <option value="aalink_font_size 38">Medium font</option>
      <option value="aalink_font_size 44">Large font</option>
      <option value="aalink_mcs_source lowest">Source Lowest LQ</option>
      <option value="aalink_mcs_source highest">Source Highest LQ</option>
      <option value="aalink_mcs_source both">Source Both LQ</option>
      <option value="aalink_mcs_source uplink">Source Uplink LQ</option>
      <option value="aalink_mcs_source downlink">Source Downlink LQ</option>
    </select>
  </div>
  
  <!-- throughput slider ---------------------------------------------------- -->
  <div class="row slider-container">
    <label for="throughputSlider">aalink Throughput %: <span id="throughputValue">50</span></label>
    <input type="range" id="throughputSlider" min="25" max="75" value="50">
  </div>

  <!-- OSD size slider ------------------------------------------------------- -->
  <div class="row slider-container">
    <label for="osdSlider">OSD Font Size: <span id="osdValue">38</span></label>
    <input type="range" id="osdSlider" min="20" max="60" step="2" value="38">
  </div>

  <!-- Reboot -->
  <div class="row">
    <button class="btn danger" style="width:100%;flex:1"
            onclick="go('reboot')">Reboot&nbsp;Device</button>
  </div>

  <!-- Show Info -->
  <div class="row">
    <button class="btn info" style="width:100%;flex:1"
            onclick="go('sysinfo')">Show Info</button>
  </div>

  <!-- Show aalink settings -->
  <div class="row">
    <button class="btn info" style="width:100%;flex:1"
            onclick="goArg('control_cmd', 'aalink_print_settings')">
      Show aalink Settings
    </button>
  </div>
  
  <pre class="info-box" id="infoControl">(no log yet)</pre>
</div>

<!-- Setup Tab Content -->
<div class="tab-content hidden" id="contentSetup">
  <!-- SSID -->
  <div class="row">
    <button class="btn" onclick="goArg('set_ssid', ssid.value)">Save&nbsp;SSID</button>
    <input id="ssid" placeholder="New SSID">
  </div>

  <!-- Password -->
  <div class="row">
    <button class="btn" onclick="goArg('set_password', pass.value)">Save&nbsp;PW</button>
    <input id="pass" placeholder="New password">
  </div>

  <!-- Channel -->
  <div class="row">
    <button class="btn" onclick="goArg('set_channel', chSel.value)">Set&nbsp;Chan</button>
    <select id="chSel">
      <option value="ch157 --make-permanent">ch157</option>
      <option value="ch149 --make-permanent">ch149</option>
      <option value="ch52 --make-permanent">ch52</option>
      <option value="ch44 --make-permanent">ch44</option>
      <option value="ch36 --make-permanent">ch36</option>
      <option value="ch157-20 --make-permanent">ch157-20</option>
      <option value="ch149-20 --make-permanent">ch149-20</option>
      <option value="ch52-20 --make-permanent">ch52-20</option>
      <option value="ch44-20 --make-permanent">ch44-20</option>
      <option value="ch36-20 --make-permanent">ch36-20</option> 
    </select>
  </div>

  <!-- WLAN power -->
  <div class="row">
    <button class="btn" onclick="goArg('set_wlanpower', pwrSel.value)">Set&nbsp;Power</button>
    <select id="pwrSel">
      <option value="pitmode">Pitmode</option>
      <option value="low">Low</option>
      <option value="medium">Medium</option>
      <option value="high">High</option>
    </select>
  </div>

  <!-- OSD -->
  <div class="row">
    <button class="btn" onclick="goArg('set_osd', ttySel.value)">Set&nbsp;OSD</button>
    <select id="ttySel">
      <option value="mode-standalone">No-tty(standalone)</option>
      <option value="mode-tty0">/dev/ttyS0</option>
      <option value="mode-tty1">/dev/ttyS1</option>
      <option value="mode-tty2">/dev/ttyS2</option>
      <option value="mode-manual">Static bitrate</option>
      <option value="mode-aalink">Adaptive bitrate</option>
    </select>
  </div>

  <!-- Video Resolution -->
  <div class="row">
    <button class="btn" onclick="goArg('set_video', videoSel.value)">Set&nbsp;Video</button>
    <select id="videoSel">
      <option value="1">4:3 720p 60</option>
      <option value="2">4:3 720p 60 50HzAC</option>
      <option value="3">4:3 960p 60</option>
      <option value="4">4:3 960p 60 50HzAC</option>
      <option value="5">4:3 1080p 60</option>
      <option value="6">4:3 1080p 60 50HzAC</option>
      <option value="7">4:3 1440p 60</option>
      <option value="8">4:3 1440p 60 50HzAC</option>
      <option value="9">4:3 1920p 60</option>
      <option value="10">4:3 1920p 60 50HzAC</option>
      <option value="11">4:3 720p 90</option>
      <option value="12">4:3 720p 90 50HzAC</option>
      <option value="13">4:3 960p 90</option>
      <option value="14">4:3 960p 90 50HzAC</option>
      <option value="15">4:3 1080p 90</option>
      <option value="16">4:3 1080p 90 50HzAC</option>
      <option value="17">4:3 1440p 90</option>
      <option value="18">4:3 1440p 90 50HzAC</option>
      <option value="19">4:3 540p 120</option>
      <option value="20">4:3 720p 120</option>
      <option value="21">4:3 960p 120</option>
      <option value="22">4:3 1080p 120</option>
      <option value="23">16:9 540p 60</option>
      <option value="24">16:9 540p 60 50HzAC</option>
      <option value="25">16:9 720p 60</option>
      <option value="26">16:9 720p 60 50HzAC</option>
      <option value="27">16:9 1080p 60</option>
      <option value="28">16:9 1080p 60 50HzAC</option>
      <option value="29">16:9 1440p 60</option>
      <option value="30">16:9 1440p 60 50HzAC</option>
      <option value="31">16:9 540p 90</option>
      <option value="32">16:9 540p 90 50HzAC</option>
      <option value="33">16:9 720p 90</option>
      <option value="34">16:9 720p 90 50HzAC</option>
      <option value="35">16:9 1080p 90</option>
      <option value="36">16:9 1080p 90 50HzAC</option>
      <option value="37">16:9 1344p 90</option>
      <option value="38">16:9 1344p 90 50HzAC</option>
      <option value="39">16:9 540p 120</option>
      <option value="40">16:9 720p 120</option>
      <option value="41">16:9 1080p 120</option>
    </select>
  </div>

  <!-- OTA -->
  <div class="row">
    <button class="btn" onclick="
      if(confirm('Warning: You are about to perform an OTA upgrade to ' + ota.options[ota.selectedIndex].text + '. Are you sure?')) {
        goArg('ota', ota.value);
      }">OTA upgrade</button>
    <select id="ota">
      <option value="ota-apfpv">AP-FPV</option>
      <option value="ota-ruby">Flash Ruby</option>
      <option value="ota-pnp">Flash &quot;PnP&quot;</option>
    </select>
  </div>

  <!-- Show Info -->
  <div class="row">
    <button class="btn info" style="width:100%;flex:1"
            onclick="go('sysinfo')">Show Info</button>
  </div>
  <pre class="info-box" id="infoSetup">(no log yet)</pre>
</div>

<script>
  // ---------------------------------------------------------------------
  // TAB SWITCHING
  // ---------------------------------------------------------------------
  const tabControl = document.getElementById('tabControl');
  const tabSetup   = document.getElementById('tabSetup');
  const contentControl = document.getElementById('contentControl');
  const contentSetup   = document.getElementById('contentSetup');

  function switchTab(toSetup) {
    if (toSetup) {
      tabSetup.classList.add('active');
      tabControl.classList.remove('active');
      contentSetup.classList.remove('hidden');
      contentControl.classList.add('hidden');
    } else {
      tabControl.classList.add('active');
      tabSetup.classList.remove('active');
      contentControl.classList.remove('hidden');
      contentSetup.classList.add('hidden');
    }
    render(lastLog);
  }

  tabControl.addEventListener('click', () => switchTab(false));
  tabSetup  .addEventListener('click', () => switchTab(true));

  // ---------------------------------------------------------------------
  // UTIL: debounce – fires at most every delay ms
  // ---------------------------------------------------------------------
  function debounce(fn, delay = 200) {
    let t;
    return (...args) => {
      clearTimeout(t);
      t = setTimeout(() => fn.apply(null, args), delay);
    };
  }

  // ---------------------------------------------------------------------
  // INFO / LOG HANDLING
  // ---------------------------------------------------------------------
  let lastLog = '(no log yet)';
  function render(txt) {
    lastLog = txt;
    document.getElementById('infoControl').textContent = txt || '(empty)';
    document.getElementById('infoSetup').textContent   = txt || '(empty)';
  }

  function updateLog() {
    fetch('/log')
      .then(r => r.ok ? r.text() : Promise.reject('HTTP ' + r.status))
      .then(render)
      .catch(e => {
        console.error('Log fetch error:', e);
        render('Log error: ' + e);
      });
  }

  function go(name) {
    fetch('/cmd/' + name)
      .then(() => updateLog())
      .catch(e => {
        console.error('Fetch error:', e);
        render('Fetch error: ' + e);
      });
  }

  function goArg(name, arg) {
    fetch('/cmd/' + name + '?args=' + encodeURIComponent(arg))
      .then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        updateLog();
      })
      .catch(e => {
        console.error('Command error:', e);
        render(`Command "${name}" failed: ${e.message}`);
      });
  }

  // ---------------------------------------------------------------------
  // THROUGHPUT SLIDER LOGIC
  // ---------------------------------------------------------------------
  const throughputSlider = document.getElementById('throughputSlider');
  const throughputValue  = document.getElementById('throughputValue');

  const sendThroughput = () =>
    goArg('control_cmd', `aalink_throughput ${throughputSlider.value}`);

  const debouncedSendThroughput = debounce(sendThroughput, 200);

  throughputSlider.addEventListener('input', () => {
    throughputValue.textContent = throughputSlider.value;
    debouncedSendThroughput();
  });
  throughputSlider.addEventListener('change', sendThroughput);

  fetch('/value/aalink_throughput')
    .then(r => r.ok ? r.text() : Promise.reject('HTTP ' + r.status))
    .then(v => {
      const val = parseInt(v, 10);
      if (!isNaN(val) && val >= 25 && val <= 75) {
        throughputSlider.value   = val;
        throughputValue.textContent = val;
      }
    })
    .catch(err => {
      console.error('Throughput slider init error:', err);
      render('Throughput slider init error: ' + err);
    });

  // ---------------------------------------------------------------------
  // OSD SIZE SLIDER LOGIC
  // ---------------------------------------------------------------------
  const osdSlider = document.getElementById('osdSlider');
  const osdValue  = document.getElementById('osdValue');

  const sendOsdSize = () =>
    goArg('control_cmd', `aalink_font_size ${osdSlider.value}`);

  const debouncedSendOsdSize = debounce(sendOsdSize, 200);

  osdSlider.addEventListener('input', () => {
    osdValue.textContent = osdSlider.value;
    debouncedSendOsdSize();
  });
  osdSlider.addEventListener('change', sendOsdSize);

  fetch('/value/aalink_font_size')
    .then(r => r.ok ? r.text() : Promise.reject('HTTP ' + r.status))
    .then(v => {
      const val = parseInt(v, 10);
      if (!isNaN(val) && val >= 20 && val <= 60) {
        osdSlider.value   = val;
        osdValue.textContent = val;
      }
    })
    .catch(err => {
      console.error('OSD slider init error:', err);
      render('OSD slider init error: ' + err);
    });

  // ---------------------------------------------------------------------
  // INITIALISE PERIODIC LOG POLL
  // ---------------------------------------------------------------------
  setInterval(updateLog, 3000);
  updateLog();
</script>
</body>
</html>
