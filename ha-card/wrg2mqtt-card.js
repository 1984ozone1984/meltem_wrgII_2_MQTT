/**
 * WRG2MQTT Custom Lovelace Card
 * Visualises and controls a Meltem M-WRG-II heat-recovery ventilation unit.
 *
 * Installation:
 *   1. Copy this file to /config/www/wrg2mqtt-card.js in Home Assistant
 *   2. In HA → Settings → Dashboards → Resources → Add resource:
 *        URL: /local/wrg2mqtt-card.js   Type: JavaScript module
 *   3. Add a card to your dashboard:
 *        type: custom:wrg2mqtt-card
 *        entity_prefix: meltem_m_wrg_ii   # adjust if HA renamed your device
 *        title: Ventilation               # optional
 */

class WRG2MQTTCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._config = {};
    this._hass   = null;
  }

  /* ── Card config ─────────────────────────────────────────────────────── */

  setConfig(config) {
    this._config = {
      title:         'M-WRG-II Ventilation',
      entity_prefix: 'meltem_m_wrg_ii',
      ...config,
    };
  }

  getCardSize() { return 10; }

  /* ── HA state updates ────────────────────────────────────────────────── */

  set hass(hass) {
    this._hass = hass;
    this._render();
  }

  /* ── Entity helpers ──────────────────────────────────────────────────── */

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

  /* ── HA service calls ────────────────────────────────────────────────── */

  _pressButton(name) {
    this._hass.callService('button', 'press', { entity_id: this._eid('button', name) });
  }

  _setNumber(name, value) {
    this._hass.callService('number', 'set_value', {
      entity_id: this._eid('number', name),
      value:     String(value),
    });
  }

  /* ── Render ──────────────────────────────────────────────────────────── */

  _render() {
    if (!this._hass) return;

    /* ----- gather state ------------------------------------------------- */
    const avail       = this._state('sensor', 'supply_air_temperature') !== 'unavailable';
    const mode        = this._state('sensor', 'operating_mode');
    const error       = this._state('binary_sensor', 'error')            === 'on';
    const filterDue   = this._state('binary_sensor', 'filter_due')       === 'on';
    const frost       = this._state('binary_sensor', 'frost_protection') === 'on';

    const tSupply   = this._num('sensor', 'supply_air_temperature');
    const tExtract  = this._num('sensor', 'extract_air_temperature');
    const tExhaust  = this._num('sensor', 'exhaust_air_temperature');
    const tOutdoor  = this._num('sensor', 'outdoor_air_temperature');

    const hExtract  = this._num('sensor', 'extract_air_humidity', 0);
    const hSupply   = this._num('sensor', 'supply_air_humidity',  0);
    const fanSup    = this._num('sensor', 'supply_fan_speed',  0);
    const fanExh    = this._num('sensor', 'exhaust_fan_speed', 0);

    const filterDays  = this._num('sensor', 'filter_days_remaining',    0);
    const hoursDev    = this._num('sensor', 'device_operating_hours',   0);
    const hoursMot    = this._num('sensor', 'motor_operating_hours',    0);

    const fanBal      = this._numRaw('number', 'manual_balanced_fan');
    const fanUSup     = this._numRaw('number', 'unbalanced_supply_fan');
    const fanUExh     = this._numRaw('number', 'unbalanced_exhaust_fan');

    const humSP       = this._numRaw('number', 'humidity_start_setpoint');
    const humMin      = this._numRaw('number', 'humidity_min_fan_level');
    const humMax      = this._numRaw('number', 'humidity_max_fan_level');
    const extFan      = this._numRaw('number', 'ext_input_fan_level');
    const extOn       = this._numRaw('number', 'ext_input_on_delay');
    const extOff      = this._numRaw('number', 'ext_input_off_delay');

    /* ----- mode presentation -------------------------------------------- */
    const MODE = {
      off:          { label: 'Off',          col: '#9e9e9e' },
      humidity:     { label: 'Humidity',     col: '#2196f3' },
      manual:       { label: 'Balanced',     col: '#4caf50' },
      manual_unbal: { label: 'Unbalanced',   col: '#ff9800' },
    };
    const m    = MODE[mode] ?? { label: mode, col: '#9e9e9e' };
    const mCol = m.col;

    /* ----- CSS ---------------------------------------------------------- */
    const css = `
      :host { display: block; }
      *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

      .card {
        background: var(--card-background-color, #1c1c1e);
        border-radius: 16px;
        padding: 16px;
        color: var(--primary-text-color, #e8e8e8);
        font-family: var(--primary-font-family, -apple-system, BlinkMacSystemFont, sans-serif);
        font-size: 14px;
      }

      /* ── header ── */
      .header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 16px;
      }
      .header-left {
        display: flex;
        align-items: center;
        gap: 8px;
        font-size: 1.05em;
        font-weight: 700;
      }
      .header-right {
        display: flex;
        align-items: center;
        gap: 6px;
      }
      .dot {
        width: 8px; height: 8px;
        border-radius: 50%;
      }
      .avail-text {
        font-size: .75em;
        font-weight: 500;
      }

      /* ── section titles ── */
      .sect {
        font-size: .65em;
        font-weight: 800;
        text-transform: uppercase;
        letter-spacing: .09em;
        color: var(--secondary-text-color, #888);
        margin: 14px 0 8px;
      }

      /* ── temperature flow diagram ── */
      .temp-flow {
        display: grid;
        grid-template-columns: 1fr 48px 1fr;
        grid-template-rows: auto 20px auto;
        gap: 0;
        background: var(--secondary-background-color, rgba(128,128,128,.08));
        border-radius: 12px;
        padding: 14px 10px;
        align-items: center;
      }
      .temp-card {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 3px;
        padding: 4px;
      }
      .temp-icon  { font-size: 1.4em; line-height: 1; }
      .temp-lbl   { font-size: .63em; color: var(--secondary-text-color,#888);
                    text-transform: uppercase; letter-spacing: .05em; }
      .temp-val   { font-size: 1.65em; font-weight: 800; line-height: 1; }
      .temp-unit  { font-size: .65em; font-weight: 400;
                    color: var(--secondary-text-color,#888); margin-left: 1px; }

      /* centre column: HEX + arrows */
      .hex-col {
        grid-column: 2;
        grid-row: 1 / 4;
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        gap: 4px;
        height: 100%;
      }
      .hex-pill {
        background: rgba(128,128,128,.15);
        border: 1px solid rgba(128,128,128,.2);
        border-radius: 6px;
        padding: 4px 6px;
        font-size: .6em;
        font-weight: 700;
        color: var(--secondary-text-color,#888);
        letter-spacing: .06em;
        writing-mode: vertical-rl;
        text-orientation: mixed;
      }
      .hex-bar {
        width: 4px;
        flex: 1;
        border-radius: 2px;
        background: linear-gradient(to bottom, #ff7043 0%, #ffa726 40%, #66bb6a 60%, #42a5f5 100%);
        min-height: 28px;
      }

      /* arrow row */
      .flow-arrows {
        grid-column: 1 / -1;
        display: flex;
        justify-content: space-between;
        align-items: center;
        padding: 0 8px;
      }
      .arrow-seg {
        display: flex;
        align-items: center;
        gap: 3px;
        font-size: .72em;
        color: var(--secondary-text-color,#666);
        flex: 1;
      }
      .arrow-seg.right { justify-content: flex-start; }
      .arrow-seg.left  { justify-content: flex-end; }
      .aline {
        height: 2px; flex: 1; max-width: 50px; border-radius: 1px;
      }

      /* ── metric grid 2-up ── */
      .grid2 {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 8px;
      }
      .mbox {
        background: var(--secondary-background-color, rgba(128,128,128,.08));
        border-radius: 10px;
        padding: 10px 12px;
        display: flex;
        flex-direction: column;
        gap: 4px;
      }
      .mlbl {
        font-size: .63em;
        color: var(--secondary-text-color,#888);
        text-transform: uppercase;
        letter-spacing: .05em;
      }
      .mval {
        font-size: 1.25em;
        font-weight: 700;
        line-height: 1;
      }
      .munit { font-size: .72em; font-weight: 400;
               color: var(--secondary-text-color,#888); margin-left: 2px; }
      .mbar-wrap {
        height: 4px;
        background: rgba(128,128,128,.15);
        border-radius: 2px;
        overflow: hidden;
        margin-top: 2px;
      }
      .mbar { height: 100%; border-radius: 2px; transition: width .5s; }

      /* ── divider ── */
      .divider {
        height: 1px;
        background: rgba(128,128,128,.12);
        margin: 14px 0;
      }

      /* ── mode badge ── */
      .mode-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 10px;
      }
      .mode-badge {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        padding: 5px 12px;
        border-radius: 20px;
        font-size: .82em;
        font-weight: 700;
        border: 1px solid ${mCol}55;
        background: ${mCol}1a;
        color: ${mCol};
      }
      .mode-dot { width: 7px; height: 7px; border-radius: 50%; background: ${mCol}; }

      /* ── buttons ── */
      .btn-row { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 12px; }
      .btn {
        flex: 1;
        min-width: 70px;
        padding: 9px 12px;
        border: none;
        border-radius: 9px;
        font-size: .82em;
        font-weight: 700;
        cursor: pointer;
        transition: all .12s;
        letter-spacing: .01em;
      }
      .btn:active { transform: scale(.96); }
      .btn-off      { background:rgba(158,158,158,.18); color:#9e9e9e;
                      border:1px solid rgba(158,158,158,.3); }
      .btn-off:hover{ background:rgba(158,158,158,.32); }
      .btn-hum      { background:rgba(33,150,243,.18); color:#2196f3;
                      border:1px solid rgba(33,150,243,.3); }
      .btn-hum:hover{ background:rgba(33,150,243,.32); }
      .btn-reboot   { background:rgba(244,67,54,.12); color:#f44336;
                      border:1px solid rgba(244,67,54,.25); width:100%; margin-top:6px; }
      .btn-reboot:hover{ background:rgba(244,67,54,.26); }

      /* ── sliders ── */
      .sl-row { margin: 9px 0; }
      .sl-head {
        display: flex;
        justify-content: space-between;
        align-items: baseline;
        margin-bottom: 5px;
      }
      .sl-name { font-size: .78em; color: var(--secondary-text-color,#888); }
      .sl-val  { font-size: .88em; font-weight: 700;
                 min-width: 64px; text-align: right; }
      input[type=range] {
        width: 100%; height: 4px;
        -webkit-appearance: none; appearance: none;
        background: rgba(128,128,128,.2);
        border-radius: 2px; outline: none; cursor: pointer;
      }
      input[type=range]::-webkit-slider-thumb {
        -webkit-appearance: none;
        width: 17px; height: 17px; border-radius: 50%;
        background: var(--primary-color, #2196f3);
        box-shadow: 0 1px 4px rgba(0,0,0,.35);
        cursor: pointer;
      }
      input[type=range]::-moz-range-thumb {
        width: 17px; height: 17px; border-radius: 50%; border: none;
        background: var(--primary-color, #2196f3);
      }

      /* ── status chips ── */
      .chip-row { display: flex; gap: 6px; flex-wrap: wrap; margin: 6px 0 10px; }
      .chip {
        display: inline-flex; align-items: center; gap: 4px;
        padding: 5px 10px; border-radius: 14px;
        font-size: .75em; font-weight: 600;
      }
      .c-ok   { background:rgba(76,175,80,.14);  color:#4caf50;
                border:1px solid rgba(76,175,80,.3); }
      .c-warn { background:rgba(255,152,0,.14);  color:#ff9800;
                border:1px solid rgba(255,152,0,.3); }
      .c-err  { background:rgba(244,67,54,.14);  color:#f44336;
                border:1px solid rgba(244,67,54,.3); }
      .c-info { background:rgba(128,128,128,.1); color:var(--secondary-text-color,#888);
                border:1px solid rgba(128,128,128,.2); }

      /* ── stat boxes ── */
      .stat-grid {
        display: grid;
        grid-template-columns: repeat(3, 1fr);
        gap: 6px;
      }
      .stat-box {
        background: var(--secondary-background-color,rgba(128,128,128,.08));
        border-radius: 9px;
        padding: 8px 6px;
        text-align: center;
      }
      .stat-val  { font-size: 1.1em; font-weight: 700; }
      .stat-unit { font-size: .62em; color: var(--secondary-text-color,#888); margin-left: 1px; }
      .stat-lbl  { font-size: .6em; color: var(--secondary-text-color,#888);
                   text-transform: uppercase; letter-spacing: .04em; margin-top: 2px; }

      /* ── collapsible sections ── */
      details { margin-top: 4px; }
      summary {
        list-style: none; cursor: pointer;
        font-size: .65em; font-weight: 800;
        text-transform: uppercase; letter-spacing: .09em;
        color: var(--secondary-text-color,#888);
        display: flex; align-items: center; gap: 6px;
        padding: 8px 0;
        border-top: 1px solid rgba(128,128,128,.1);
        user-select: none;
      }
      summary::-webkit-details-marker { display: none; }
      summary::before {
        content: '▶'; font-size: .75em;
        transition: transform .18s;
        display: inline-block;
      }
      details[open] > summary::before { transform: rotate(90deg); }
      .details-inner { padding: 6px 0 4px; }
    `;

    /* ----- HTML --------------------------------------------------------- */
    const html = `
      <div class="card">

        <!-- HEADER -->
        <div class="header">
          <div class="header-left">
            <span>🌀</span>
            <span>${this._config.title}</span>
          </div>
          <div class="header-right">
            <div class="dot" style="background:${avail ? '#4caf50' : '#f44336'};
              box-shadow:0 0 6px ${avail ? '#4caf50' : '#f44336'}"></div>
            <span class="avail-text" style="color:${avail ? '#4caf50' : '#f44336'}">
              ${avail ? 'Online' : 'Offline'}
            </span>
          </div>
        </div>

        <!-- TEMPERATURES -->
        <div class="sect">Temperatures</div>
        <div class="temp-flow">

          <!-- top-left: Outdoor -->
          <div class="temp-card">
            <div class="temp-icon">🌤</div>
            <div class="temp-lbl">Outdoor</div>
            <div class="temp-val" style="color:#42a5f5">
              ${tOutdoor}<span class="temp-unit">°C</span>
            </div>
          </div>

          <!-- centre column spanning all 3 rows -->
          <div class="hex-col">
            <div class="hex-pill">HEX</div>
            <div class="hex-bar"></div>
            <div class="hex-pill">HEX</div>
          </div>

          <!-- top-right: Supply -->
          <div class="temp-card">
            <div class="temp-icon">🏠</div>
            <div class="temp-lbl">Supply</div>
            <div class="temp-val" style="color:#66bb6a">
              ${tSupply}<span class="temp-unit">°C</span>
            </div>
          </div>

          <!-- middle row: flow arrows -->
          <div class="flow-arrows" style="grid-column:1/-1">
            <div class="arrow-seg right">
              <div class="aline"
                style="background:linear-gradient(to right,#42a5f5,#66bb6a)"></div>
              <span>→</span>
            </div>
            <div style="width:48px"></div>
            <div class="arrow-seg left">
              <span>←</span>
              <div class="aline"
                style="background:linear-gradient(to left,#ff7043,#ffa726)"></div>
            </div>
          </div>

          <!-- bottom-left: Extract -->
          <div class="temp-card">
            <div class="temp-icon">💨</div>
            <div class="temp-lbl">Extract</div>
            <div class="temp-val" style="color:#ffa726">
              ${tExtract}<span class="temp-unit">°C</span>
            </div>
          </div>

          <!-- bottom-right: Exhaust -->
          <div class="temp-card">
            <div class="temp-icon">🔄</div>
            <div class="temp-lbl">Exhaust</div>
            <div class="temp-val" style="color:#ff7043">
              ${tExhaust}<span class="temp-unit">°C</span>
            </div>
          </div>

        </div>

        <!-- AIR QUALITY -->
        <div class="sect">Air Quality &amp; Fan Speed</div>
        <div class="grid2">
          <div class="mbox">
            <div class="mlbl">Extract Humidity</div>
            <div class="mval">${hExtract}<span class="munit">%</span></div>
            <div class="mbar-wrap">
              <div class="mbar" style="width:${hExtract}%;background:#2196f3"></div>
            </div>
          </div>
          <div class="mbox">
            <div class="mlbl">Supply Humidity</div>
            <div class="mval">${hSupply}<span class="munit">%</span></div>
            <div class="mbar-wrap">
              <div class="mbar" style="width:${hSupply}%;background:#42a5f5"></div>
            </div>
          </div>
          <div class="mbox">
            <div class="mlbl">Supply Fan</div>
            <div class="mval">${fanSup}<span class="munit">m³/h</span></div>
            <div class="mbar-wrap">
              <div class="mbar"
                style="width:${Math.min(100, fanSup)}%;background:#4caf50"></div>
            </div>
          </div>
          <div class="mbox">
            <div class="mlbl">Exhaust Fan</div>
            <div class="mval">${fanExh}<span class="munit">m³/h</span></div>
            <div class="mbar-wrap">
              <div class="mbar"
                style="width:${Math.min(100, fanExh)}%;background:#66bb6a"></div>
            </div>
          </div>
        </div>

        <div class="divider"></div>

        <!-- MODE & CONTROLS -->
        <div class="mode-row">
          <div class="sect" style="margin:0">Controls</div>
          <div class="mode-badge">
            <div class="mode-dot"></div>
            ${m.label}
          </div>
        </div>

        <div class="btn-row">
          <button class="btn btn-off" id="btn-off">⏹ Switch Off</button>
          <button class="btn btn-hum" id="btn-hum">💧 Humidity</button>
        </div>

        ${this._sliderHtml('sl-bal',  'lbl-bal',  '⚖ Balanced Fan',            0, 100, 5, fanBal,  'm³/h')}
        ${this._sliderHtml('sl-sup',  'lbl-sup',  '→ Supply Fan (Unbalanced)',  0, 100, 5, fanUSup, 'm³/h')}
        ${this._sliderHtml('sl-exh',  'lbl-exh',  '← Exhaust Fan (Unbalanced)',0, 100, 5, fanUExh, 'm³/h')}

        <div class="divider"></div>

        <!-- STATUS -->
        <div class="sect">Status &amp; Maintenance</div>
        <div class="chip-row">
          <span class="chip ${error     ? 'c-err'  : 'c-ok'}">${error     ? '⚠ Error'      : '✓ OK'}</span>
          <span class="chip ${filterDue ? 'c-warn' : 'c-ok'}">${filterDue ? '⚠ Filter Due'  : '✓ Filter OK'}</span>
          <span class="chip ${frost     ? 'c-warn' : 'c-info'}">${frost   ? '❄ Frost Active' : '❄ No Frost'}</span>
        </div>
        <div class="stat-grid">
          <div class="stat-box">
            <div class="stat-val">${filterDays}<span class="stat-unit">d</span></div>
            <div class="stat-lbl">Filter Left</div>
          </div>
          <div class="stat-box">
            <div class="stat-val">${hoursDev}<span class="stat-unit">h</span></div>
            <div class="stat-lbl">Device Hrs</div>
          </div>
          <div class="stat-box">
            <div class="stat-val">${hoursMot}<span class="stat-unit">h</span></div>
            <div class="stat-lbl">Motor Hrs</div>
          </div>
        </div>

        <!-- HUMIDITY CONFIG (collapsible) -->
        <details>
          <summary>Humidity Control Config</summary>
          <div class="details-inner">
            ${this._sliderHtml('sl-hsp',  'lbl-hsp',  'Start Setpoint', 40, 80,  1, humSP,  '%')}
            ${this._sliderHtml('sl-hmin', 'lbl-hmin', 'Min Fan Level',  0,  100, 1, humMin, '%')}
            ${this._sliderHtml('sl-hmax', 'lbl-hmax', 'Max Fan Level',  0,  100, 1, humMax, '%')}
          </div>
        </details>

        <!-- EXT INPUT CONFIG (collapsible) -->
        <details>
          <summary>External Input Config</summary>
          <div class="details-inner">
            ${this._sliderHtml('sl-efl',  'lbl-efl',  'Fan Level', 0, 100, 1, extFan, '%')}
            ${this._sliderHtml('sl-eon',  'lbl-eon',  'On Delay',  0,  60, 1, extOn,  'min')}
            ${this._sliderHtml('sl-eoff', 'lbl-eoff', 'Off Delay', 0, 120, 1, extOff, 'min')}
          </div>
        </details>

        <!-- DEVICE (collapsible) -->
        <details>
          <summary>Device</summary>
          <div class="details-inner">
            <button class="btn btn-reboot" id="btn-reboot">⟳ Reboot Device</button>
          </div>
        </details>

      </div>
    `;

    this.shadowRoot.innerHTML = `<style>${css}</style>${html}`;
    this._bindEvents();
  }

  /* ── Slider HTML helper ──────────────────────────────────────────────── */

  _sliderHtml(id, lblId, name, min, max, step, value, unit) {
    return `
      <div class="sl-row">
        <div class="sl-head">
          <span class="sl-name">${name}</span>
          <span class="sl-val" id="${lblId}">${value} ${unit}</span>
        </div>
        <input type="range" id="${id}"
          min="${min}" max="${max}" step="${step}" value="${value}">
      </div>`;
  }

  /* ── Event binding ───────────────────────────────────────────────────── */

  _bindEvents() {
    const r = this.shadowRoot;

    r.getElementById('btn-off')?.addEventListener('click', () =>
      this._pressButton('switch_off'));

    r.getElementById('btn-hum')?.addEventListener('click', () =>
      this._pressButton('humidity_control'));

    r.getElementById('btn-reboot')?.addEventListener('click', () => {
      if (confirm('Reboot the ventilation unit?'))
        this._pressButton('reboot');
    });

    /* fan sliders */
    this._bindSlider('sl-bal',  'lbl-bal',  v => `${v} m³/h`,
      v => this._setNumber('manual_balanced_fan',    v));
    this._bindSlider('sl-sup',  'lbl-sup',  v => `${v} m³/h`,
      v => this._setNumber('unbalanced_supply_fan',  v));
    this._bindSlider('sl-exh',  'lbl-exh',  v => `${v} m³/h`,
      v => this._setNumber('unbalanced_exhaust_fan', v));

    /* humidity config sliders */
    this._bindSlider('sl-hsp',  'lbl-hsp',  v => `${v} %`,
      v => this._setNumber('humidity_start_setpoint', v));
    this._bindSlider('sl-hmin', 'lbl-hmin', v => `${v} %`,
      v => this._setNumber('humidity_min_fan_level',  v));
    this._bindSlider('sl-hmax', 'lbl-hmax', v => `${v} %`,
      v => this._setNumber('humidity_max_fan_level',  v));

    /* ext input config sliders */
    this._bindSlider('sl-efl',  'lbl-efl',  v => `${v} %`,
      v => this._setNumber('ext_input_fan_level', v));
    this._bindSlider('sl-eon',  'lbl-eon',  v => `${v} min`,
      v => this._setNumber('ext_input_on_delay',  v));
    this._bindSlider('sl-eoff', 'lbl-eoff', v => `${v} min`,
      v => this._setNumber('ext_input_off_delay', v));
  }

  _bindSlider(sliderId, labelId, fmt, onRelease) {
    const r      = this.shadowRoot;
    const slider = r.getElementById(sliderId);
    const label  = r.getElementById(labelId);
    if (!slider || !label) return;

    /* live label while dragging */
    slider.addEventListener('input', () => {
      label.textContent = fmt(slider.value);
    });

    /* send to HA only on release */
    slider.addEventListener('change', () => {
      onRelease(parseFloat(slider.value));
    });
  }
}

customElements.define('wrg2mqtt-card', WRG2MQTTCard);
