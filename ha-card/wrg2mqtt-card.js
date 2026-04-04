/**
 * WRG2MQTT Custom Lovelace Card — visual style inspired by Meltem dashboard
 *
 * Installation:
 *   1. Copy to /config/www/wrg2mqtt-card.js
 *   2. HA → Settings → Dashboards → Resources → Add:
 *        URL: /local/wrg2mqtt-card.js   Type: JavaScript module
 *   3. Add card:
 *        type: custom:wrg2mqtt-card
 *        entity_prefix: meltem_m_wrg_ii
 */

class WRG2MQTTCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
  }

  setConfig(config) {
    this._config = {
      title: 'M-WRG-II',
      entity_prefix: 'meltem_m_wrg_ii',
      ...config,
    };
  }

  getCardSize() { return 8; }

  set hass(hass) {
    this._hass = hass;
    this._render();
  }

  _eid(domain, name) {
    return `${domain}.${this._config.entity_prefix}_${name}`;
  }
  _state(domain, name) {
    return this._hass?.states[this._eid(domain, name)]?.state ?? 'unavailable';
  }
  _num(domain, name, dec = 1) {
    const v = parseFloat(this._state(domain, name));
    return isNaN(v) ? '--' : v.toFixed(dec);
  }
  _numRaw(domain, name) {
    const v = parseFloat(this._state(domain, name));
    return isNaN(v) ? 0 : v;
  }
  _pressButton(name) {
    this._hass.callService('button', 'press', { entity_id: this._eid('button', name) });
  }
  _setNumber(name, value) {
    this._hass.callService('number', 'set_value', {
      entity_id: this._eid('number', name), value: String(value),
    });
  }

  _render() {
    if (!this._hass) return;

    const avail      = this._state('sensor', 'supply_air_temperature') !== 'unavailable';
    const mode       = this._state('sensor', 'operating_mode');
    const error      = this._state('binary_sensor', 'error')            === 'on';
    const filterDue  = this._state('binary_sensor', 'filter_due')       === 'on';
    const frost      = this._state('binary_sensor', 'frost_protection') === 'on';

    const tOutdoor  = this._num('sensor', 'outdoor_air_temperature');
    const tExtract  = this._num('sensor', 'extract_air_temperature');
    const tSupply   = this._num('sensor', 'supply_air_temperature');
    const tExhaust  = this._num('sensor', 'exhaust_air_temperature');

    const hExtract  = this._num('sensor', 'extract_air_humidity', 0);
    const hSupply   = this._num('sensor', 'supply_air_humidity',  0);
    const fanSup    = this._num('sensor', 'supply_fan_speed',  0);
    const fanExh    = this._num('sensor', 'exhaust_fan_speed', 0);

    const filterDays = this._num('sensor', 'filter_days_remaining', 0);
    const hoursDev   = this._num('sensor', 'device_operating_hours', 0);
    const hoursMot   = this._num('sensor', 'motor_operating_hours',  0);

    const fanBal     = this._numRaw('number', 'manual_balanced_fan');
    const fanUSup    = this._numRaw('number', 'unbalanced_supply_fan');
    const fanUExh    = this._numRaw('number', 'unbalanced_exhaust_fan');
    const humSP      = this._numRaw('number', 'humidity_start_setpoint');
    const humMin     = this._numRaw('number', 'humidity_min_fan_level');
    const humMax     = this._numRaw('number', 'humidity_max_fan_level');
    const extFan     = this._numRaw('number', 'ext_input_fan_level');
    const extOn      = this._numRaw('number', 'ext_input_on_delay');
    const extOff     = this._numRaw('number', 'ext_input_off_delay');

    const MODE = {
      off:          { label: 'Off',        sub: 'Ventilation off',             col: '#607d8b', icon: '⏹' },
      humidity:     { label: 'Humidity',   sub: 'Automatic humidity control',  col: '#1976d2', icon: '💧' },
      manual:       { label: 'Balanced',   sub: 'Manual balanced fan',         col: '#388e3c', icon: '⚖' },
      manual_unbal: { label: 'Unbalanced', sub: 'Manual unbalanced fan',       col: '#f57c00', icon: '↕' },
    };
    const m = MODE[mode] ?? { label: mode, sub: '', col: '#607d8b', icon: '?' };

    /* Fan speed display — show balanced or unbalanced depending on mode */
    const showBal   = (mode === 'manual');
    const showUnbal = (mode === 'manual_unbal');
    const leftFlow  = showUnbal ? `${fanUSup} m³/h` : `${fanSup} m³/h`;
    const rightFlow = showUnbal ? `${fanUExh} m³/h` : `${fanExh} m³/h`;

    const css = `
      :host { display: block; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }
      * { box-sizing: border-box; margin: 0; padding: 0; }

      /* ── top panel: white / light ── */
      .top {
        background: #ffffff;
        border-radius: 16px 16px 0 0;
        padding: 18px 16px 14px;
        position: relative;
        overflow: hidden;
      }

      .top-header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 4px;
      }
      .device-name {
        font-size: .8em;
        font-weight: 700;
        color: #555;
        text-transform: uppercase;
        letter-spacing: .08em;
      }
      .avail-chip {
        font-size: .7em;
        font-weight: 600;
        padding: 2px 8px;
        border-radius: 10px;
        background: ${avail ? '#e8f5e9' : '#ffebee'};
        color: ${avail ? '#2e7d32' : '#c62828'};
      }

      /* ── flow diagram ── */
      .flow {
        display: grid;
        grid-template-columns: 140px 1fr 140px;
        grid-template-rows: 1fr;
        align-items: center;
        min-height: 180px;
        position: relative;
        gap: 0;
      }

      /* left column: outdoor side */
      .side { display: flex; flex-direction: column; gap: 6px; }
      .side.right { align-items: flex-end; text-align: right; }

      .circle {
        width: 64px; height: 64px;
        border-radius: 50%;
        display: flex; align-items: center; justify-content: center;
        font-size: 1.8em;
        flex-shrink: 0;
      }
      .circle-blue { background: #1976d2; color: #fff; }
      .circle-red  { background: #c62828; color: #fff; }

      .side-row { display: flex; align-items: center; gap: 8px; }
      .side.right .side-row { flex-direction: row-reverse; }

      .temp-line {
        font-size: .85em;
        font-weight: 700;
        color: #222;
      }
      .hum-line {
        font-size: .78em;
        color: #555;
      }
      .side-label {
        font-size: .68em;
        font-weight: 700;
        color: #888;
        text-transform: uppercase;
        letter-spacing: .06em;
      }

      /* ── centre: hexagon + arrows SVG ── */
      .centre {
        position: relative;
        display: flex;
        align-items: center;
        justify-content: center;
      }
      .centre svg { width: 100%; max-width: 240px; height: auto; }

      .hex-info {
        position: absolute;
        display: flex;
        flex-direction: column;
        align-items: center;
        pointer-events: none;
      }
      .hex-days {
        font-size: .72em;
        font-weight: 600;
        color: #666;
      }
      .hex-mode {
        font-size: .9em;
        font-weight: 800;
        color: #333;
      }

      /* flow speed badges */
      .flow-badge {
        position: absolute;
        font-size: .75em;
        font-weight: 800;
        color: #fff;
        background: rgba(0,0,0,.35);
        padding: 2px 7px;
        border-radius: 10px;
        pointer-events: none;
        white-space: nowrap;
      }
      .badge-left  { left: 6%;  top: 62%; }
      .badge-right { right: 6%; top: 35%; }

      /* status strip */
      .status-strip {
        display: flex;
        gap: 6px;
        justify-content: center;
        flex-wrap: wrap;
        margin-top: 10px;
        padding-top: 10px;
        border-top: 1px solid #f0f0f0;
      }
      .s-chip {
        font-size: .7em;
        font-weight: 600;
        padding: 3px 9px;
        border-radius: 10px;
        display: inline-flex;
        align-items: center;
        gap: 3px;
      }
      .s-ok   { background:#e8f5e9; color:#2e7d32; }
      .s-warn { background:#fff3e0; color:#e65100; }
      .s-err  { background:#ffebee; color:#c62828; }
      .s-info { background:#f5f5f5; color:#757575; }

      .stat-row {
        display: flex;
        justify-content: center;
        gap: 16px;
        margin-top: 8px;
      }
      .stat-item { text-align: center; }
      .stat-val  { font-size: .9em; font-weight: 700; color: #333; }
      .stat-lbl  { font-size: .6em; color: #999; text-transform: uppercase;
                   letter-spacing: .05em; }

      /* ── bottom panel: dark tiles ── */
      .bottom {
        background: #1a1a2e;
        border-radius: 0 0 16px 16px;
        padding: 12px;
        display: flex;
        flex-direction: column;
        gap: 8px;
      }

      /* mode tiles row */
      .tiles {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 8px;
      }
      .tiles.three { grid-template-columns: 1fr 1fr 1fr; }

      .tile {
        border-radius: 10px;
        padding: 12px 10px;
        cursor: pointer;
        transition: all .15s;
        display: flex;
        flex-direction: column;
        align-items: flex-start;
        gap: 4px;
        border: 2px solid transparent;
        position: relative;
        user-select: none;
      }
      .tile:active { transform: scale(.97); }
      .tile-icon {
        font-size: 1.3em;
        margin-bottom: 2px;
      }
      .tile-title {
        font-size: .78em;
        font-weight: 700;
        color: #fff;
        line-height: 1.2;
      }
      .tile-sub {
        font-size: .65em;
        color: rgba(255,255,255,.5);
        line-height: 1.3;
      }
      .tile-active {
        border-color: rgba(255,255,255,.35) !important;
        box-shadow: 0 0 12px rgba(255,255,255,.1);
      }

      .tile-off      { background: #2d3436; }
      .tile-humidity { background: #1a3a5c; }
      .tile-bal      { background: #1b3a2a; }
      .tile-reboot   { background: #3a1a1a; }

      /* ── sliders section ── */
      .sliders-wrap {
        background: #16213e;
        border-radius: 10px;
        padding: 12px;
      }
      .sl-title {
        font-size: .65em;
        font-weight: 700;
        text-transform: uppercase;
        letter-spacing: .08em;
        color: rgba(255,255,255,.4);
        margin-bottom: 10px;
      }
      .sl-row { margin: 8px 0; }
      .sl-head {
        display: flex;
        justify-content: space-between;
        align-items: baseline;
        margin-bottom: 4px;
      }
      .sl-name { font-size: .76em; color: rgba(255,255,255,.6); }
      .sl-val  { font-size: .82em; font-weight: 700; color: #fff; }
      input[type=range] {
        width: 100%; height: 4px;
        -webkit-appearance: none; appearance: none;
        background: rgba(255,255,255,.15);
        border-radius: 2px; outline: none; cursor: pointer;
      }
      input[type=range]::-webkit-slider-thumb {
        -webkit-appearance: none;
        width: 16px; height: 16px; border-radius: 50%;
        background: #fff;
        box-shadow: 0 1px 4px rgba(0,0,0,.4);
      }
      input[type=range]::-moz-range-thumb {
        width: 16px; height: 16px; border-radius: 50%; border: none;
        background: #fff;
      }

      /* collapsible config */
      details { }
      summary {
        list-style: none; cursor: pointer;
        font-size: .65em; font-weight: 700;
        text-transform: uppercase; letter-spacing: .08em;
        color: rgba(255,255,255,.35);
        display: flex; align-items: center; gap: 5px;
        padding: 6px 0;
        user-select: none;
      }
      summary::-webkit-details-marker { display: none; }
      summary::before {
        content: '▶'; font-size: .75em;
        transition: transform .18s; display: inline-block;
      }
      details[open] > summary::before { transform: rotate(90deg); }
    `;

    /* ── SVG: hexagon + crossing arrows ── */
    const svg = `
      <svg viewBox="0 0 260 180" xmlns="http://www.w3.org/2000/svg">
        <defs>
          <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
            <feDropShadow dx="0" dy="2" stdDeviation="4" flood-opacity="0.2"/>
          </filter>
          <!-- blue arrow marker -->
          <marker id="ab" markerWidth="8" markerHeight="6"
            refX="7" refY="3" orient="auto">
            <polygon points="0 0, 8 3, 0 6" fill="#1565c0" opacity=".85"/>
          </marker>
          <!-- red arrow marker -->
          <marker id="ar" markerWidth="8" markerHeight="6"
            refX="7" refY="3" orient="auto">
            <polygon points="0 0, 8 3, 0 6" fill="#b71c1c" opacity=".85"/>
          </marker>
        </defs>

        <!-- Blue arrow: outdoor (left) → supply (right), top arc -->
        <path d="M 10,110 C 60,110 80,50 130,90 C 175,125 200,60 250,60"
          stroke="#1565c0" stroke-opacity=".8" stroke-width="20"
          fill="none" stroke-linecap="round"
          marker-end="url(#ab)" opacity=".75"/>

        <!-- Red arrow: extract (right) → exhaust (left), bottom arc -->
        <path d="M 250,120 C 200,120 175,175 130,90 C 80,45 60,130 10,70"
          stroke="#b71c1c" stroke-opacity=".8" stroke-width="20"
          fill="none" stroke-linecap="round"
          marker-end="url(#ar)" opacity=".75"/>

        <!-- Hexagon -->
        <polygon
          points="130,30 168,52 168,95 130,117 92,95 92,52"
          fill="#d0d0d0" stroke="#bbb" stroke-width="2" filter="url(#shadow)"/>
        <polygon
          points="130,38 162,57 162,93 130,112 98,93 98,57"
          fill="#e8e8e8" stroke="#ccc" stroke-width="1"/>
      </svg>
    `;

    const sliderHtml = (id, lblId, name, min, max, step, val, unit) => `
      <div class="sl-row">
        <div class="sl-head">
          <span class="sl-name">${name}</span>
          <span class="sl-val" id="${lblId}">${val} ${unit}</span>
        </div>
        <input type="range" id="${id}" min="${min}" max="${max}"
          step="${step}" value="${val}">
      </div>`;

    const html = `
      <!-- ══ TOP PANEL ══ -->
      <div class="top">
        <div class="top-header">
          <span class="device-name">${this._config.title}</span>
          <span class="avail-chip">${avail ? '● Online' : '● Offline'}</span>
        </div>

        <div class="flow">

          <!-- LEFT: Outdoor side -->
          <div class="side">
            <div class="side-row">
              <div class="circle circle-blue">☁️</div>
              <div>
                <div class="side-label">Outdoor</div>
                <div class="temp-line">🌡 ${tOutdoor} °C</div>
                <div class="hum-line">💧 ${hExtract} %</div>
              </div>
            </div>
            <div style="margin-top:6px">
              <div class="side-label">Exhaust</div>
              <div class="temp-line">🌡 ${tExhaust} °C</div>
            </div>
          </div>

          <!-- CENTRE: SVG + overlay text -->
          <div class="centre">
            ${svg}
            <div class="hex-info">
              <div class="hex-days">🔧 ${filterDays} d</div>
              <div class="hex-mode">${m.label}</div>
            </div>
            <div class="flow-badge badge-left">${leftFlow}</div>
            <div class="flow-badge badge-right">${rightFlow}</div>
          </div>

          <!-- RIGHT: Indoor side -->
          <div class="side right">
            <div class="side-row">
              <div class="circle circle-red">🏠</div>
              <div style="text-align:right">
                <div class="side-label">Extract</div>
                <div class="temp-line">🌡 ${tExtract} °C</div>
                <div class="hum-line">💧 ${hSupply} %</div>
              </div>
            </div>
            <div style="margin-top:6px;text-align:right">
              <div class="side-label">Supply</div>
              <div class="temp-line">🌡 ${tSupply} °C</div>
            </div>
          </div>

        </div>

        <!-- status strip -->
        <div class="status-strip">
          <span class="s-chip ${error     ? 's-err'  : 's-ok'}">${error     ? '⚠ Error'      : '✓ OK'}</span>
          <span class="s-chip ${filterDue ? 's-warn' : 's-ok'}">${filterDue ? '⚠ Filter Due'  : '✓ Filter OK'}</span>
          <span class="s-chip ${frost     ? 's-warn' : 's-info'}">${frost   ? '❄ Frost Active' : '❄ No Frost'}</span>
          <span class="s-chip s-info">⏱ Dev ${hoursDev} h</span>
          <span class="s-chip s-info">⚙ Mot ${hoursMot} h</span>
        </div>
      </div>

      <!-- ══ BOTTOM PANEL ══ -->
      <div class="bottom">

        <!-- mode tiles row 1: Off + Humidity -->
        <div class="tiles">
          <div class="tile tile-off ${mode==='off' ? 'tile-active' : ''}" id="tile-off">
            <div class="tile-icon">⏹</div>
            <div class="tile-title">Switch Off</div>
            <div class="tile-sub">Turn ventilation off</div>
          </div>
          <div class="tile tile-humidity ${mode==='humidity' ? 'tile-active' : ''}" id="tile-hum">
            <div class="tile-icon">💧</div>
            <div class="tile-title">Humidity Control</div>
            <div class="tile-sub">Automatic humidity regulation</div>
          </div>
        </div>

        <!-- fan sliders -->
        <div class="sliders-wrap">
          <div class="sl-title">Fan Control</div>
          ${sliderHtml('sl-bal',  'lbl-bal',  '⚖ Balanced',        0, 100, 5, fanBal,  'm³/h')}
          ${sliderHtml('sl-sup',  'lbl-sup',  '→ Unbal. Supply',   0, 100, 5, fanUSup, 'm³/h')}
          ${sliderHtml('sl-exh',  'lbl-exh',  '← Unbal. Exhaust',  0, 100, 5, fanUExh, 'm³/h')}
        </div>

        <!-- collapsible: humidity config -->
        <details>
          <summary>Humidity Config</summary>
          <div class="sliders-wrap" style="margin-top:6px">
            ${sliderHtml('sl-hsp',  'lbl-hsp',  'Start Setpoint', 40, 80,  1, humSP,  '%')}
            ${sliderHtml('sl-hmin', 'lbl-hmin', 'Min Fan Level',  0,  100, 1, humMin, '%')}
            ${sliderHtml('sl-hmax', 'lbl-hmax', 'Max Fan Level',  0,  100, 1, humMax, '%')}
          </div>
        </details>

        <!-- collapsible: ext input config -->
        <details>
          <summary>External Input Config</summary>
          <div class="sliders-wrap" style="margin-top:6px">
            ${sliderHtml('sl-efl',  'lbl-efl',  'Fan Level', 0, 100, 1, extFan, '%')}
            ${sliderHtml('sl-eon',  'lbl-eon',  'On Delay',  0,  60, 1, extOn,  'min')}
            ${sliderHtml('sl-eoff', 'lbl-eoff', 'Off Delay', 0, 120, 1, extOff, 'min')}
          </div>
        </details>

        <!-- collapsible: device -->
        <details>
          <summary>Device</summary>
          <div style="padding:8px 0">
            <div class="tile tile-reboot" id="tile-reboot"
              style="flex-direction:row;align-items:center;gap:10px;padding:10px 14px">
              <div class="tile-icon" style="margin:0">⟳</div>
              <div>
                <div class="tile-title">Reboot Device</div>
                <div class="tile-sub">Restart the ESP32</div>
              </div>
            </div>
          </div>
        </details>

      </div>
    `;

    this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;
    this._bind();
  }

  _bind() {
    const r = this.shadowRoot;

    r.getElementById('tile-off')?.addEventListener('click', () =>
      this._pressButton('switch_off'));
    r.getElementById('tile-hum')?.addEventListener('click', () =>
      this._pressButton('humidity_control'));
    r.getElementById('tile-reboot')?.addEventListener('click', () => {
      if (confirm('Reboot the ventilation unit?'))
        this._pressButton('reboot');
    });

    this._sl('sl-bal',  'lbl-bal',  v => `${v} m³/h`, v => this._setNumber('manual_balanced_fan',    v));
    this._sl('sl-sup',  'lbl-sup',  v => `${v} m³/h`, v => this._setNumber('unbalanced_supply_fan',  v));
    this._sl('sl-exh',  'lbl-exh',  v => `${v} m³/h`, v => this._setNumber('unbalanced_exhaust_fan', v));
    this._sl('sl-hsp',  'lbl-hsp',  v => `${v} %`,    v => this._setNumber('humidity_start_setpoint', v));
    this._sl('sl-hmin', 'lbl-hmin', v => `${v} %`,    v => this._setNumber('humidity_min_fan_level',  v));
    this._sl('sl-hmax', 'lbl-hmax', v => `${v} %`,    v => this._setNumber('humidity_max_fan_level',  v));
    this._sl('sl-efl',  'lbl-efl',  v => `${v} %`,    v => this._setNumber('ext_input_fan_level', v));
    this._sl('sl-eon',  'lbl-eon',  v => `${v} min`,  v => this._setNumber('ext_input_on_delay',  v));
    this._sl('sl-eoff', 'lbl-eoff', v => `${v} min`,  v => this._setNumber('ext_input_off_delay', v));
  }

  _sl(sid, lid, fmt, onRelease) {
    const r = this.shadowRoot;
    const s = r.getElementById(sid);
    const l = r.getElementById(lid);
    if (!s || !l) return;
    s.addEventListener('input',  () => { l.textContent = fmt(s.value); });
    s.addEventListener('change', () => { onRelease(parseFloat(s.value)); });
  }
}

customElements.define('wrg2mqtt-card', WRG2MQTTCard);
