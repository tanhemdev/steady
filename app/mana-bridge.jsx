
/* Steady: hosts the team's Steady phone app (stored beside this component as `html`) in a same-origin frame and bridges it to Mana:
   - live sensor relay: the owner's connected device posts the stream; it is written to `live` and pushed to every viewer's frame
   - saved sensor sessions for the doctor report live under `sessions/<id>` (owner writes, everyone the app is shared with reads)
   Frame permissions: Bluetooth, USB, microphone, web share (PDF/CSV to the doctor). */
function UserDiv() {
  const frame = React.useRef(null);
  const [html, setHtml] = React.useState(null);
  const [err, setErr] = React.useState(null);
  const isOwner = !!userId && userId === ownerId;
  /* natural voice: ElevenLabs through Mana (owner's credits); every clip is cached in app storage so it plays instantly next time,
     also for logged-out visitors */
  const { url: elUrl } = useManaRouter('elevenlabs');
  const store = useStorageUrl('voice');
  const { url: anUrl } = useManaRouter('anthropic');
  const elRef = React.useRef(null), storeRef = React.useRef(null), anRef = React.useRef(null);
  elRef.current = elUrl; storeRef.current = store; anRef.current = anUrl;
  const addr = (...p) => (typeof relPathAddress === 'function' ? relPathAddress(...p) : null);

  React.useEffect(() => {
    const a = addr('html');
    if (!a) { setErr('Open Steady from its page in Mana.'); return; }
    FS.get(FS.ref(FS.db, a)).then(s => { const v = s.val(); if (v) setHtml(v); else setErr('Steady’s app file is missing.'); }).catch(e => setErr(String(e && e.message || e)));
  }, []);

  React.useEffect(() => {
    if (!html) return;
    const latest = { sessions: [], live: null, voice: {} };
    const sendAudio = (id, buf) => { const w = frame.current && frame.current.contentWindow; if (w) w.postMessage({ steady: 'ttsAudio', data: { id, buf } }, '*', [buf]); };
    /* speech check: the recorded attempt -> ElevenLabs Scribe -> text (nothing is stored) */
    const stt = async ({ id, buf, mime }) => {
      try {
        if (!elRef.current) throw new Error('voice service not ready');
        const ext = /mp4|aac|m4a/.test(mime || '') ? 'm4a' : /ogg/.test(mime || '') ? 'ogg' : 'webm';
        const fd = new FormData();
        fd.append('model_id', 'scribe_v2'); fd.append('language_code', 'en'); fd.append('tag_audio_events', 'false');
        fd.append('file', new Blob([buf], { type: mime || 'audio/webm' }), 'attempt.' + ext);
        const r = await fetch(elRef.current + '/v1/speech-to-text', { method: 'POST', body: fd });
        if (!r.ok) throw new Error('stt ' + r.status);
        const j = await r.json();
        post('sttResult', { id, text: j.text || '' });
      } catch (e) { post('sttResult', { id, error: String(e && e.message || e) }); }
    };
    /* care-team note and family update for the report: Claude Haiku through Mana (owner's credits) */
    const claude = async ({ id, system, user }) => {
      try {
        if (!anRef.current) throw new Error('claude not ready');
        const r = await fetch(anRef.current + '/v1/messages', { method: 'POST', headers: { 'content-type': 'application/json', 'anthropic-version': '2023-06-01' },
          body: JSON.stringify({ model: 'claude-haiku-4-5', max_tokens: 500, system: String(system || '').slice(0, 4000), messages: [{ role: 'user', content: String(user || '').slice(0, 6000) }] }) });
        if (!r.ok) throw new Error('claude ' + r.status);
        const j = await r.json();
        post('claudeResult', { id, text: (j.content || []).map(c => c.text || '').join('') });
      } catch (e) { post('claudeResult', { id, error: String(e && e.message || e) }); }
    };
    const tts = async ({ id, text, voice, key }) => {
      try {
        let buf = null;
        const st = storeRef.current || {}, rec = latest.voice[key];
        if (rec && rec.path && st.url) { try { const r = await fetch(st.url + (st.encodePath ? st.encodePath(rec.path) : rec.path)); if (r.ok) buf = await r.arrayBuffer(); } catch (e) {} }
        if (!buf && elRef.current && text && voice && voice !== 'device') {
          const r = await fetch(elRef.current + '/v1/text-to-speech/' + voice + '?output_format=mp3_44100_128', { method: 'POST', headers: { 'content-type': 'application/json' },
            body: JSON.stringify({ text, model_id: 'eleven_v4_turbo', voice_settings: { stability: 0.6, similarity_boost: 0.8, style: 0.25, use_speaker_boost: true, speed: 0.88 } }) });
          if (r.ok) {
            buf = await r.arrayBuffer();
            if (isOwner && st.upload && !latest.voice[key]) {
              const name = key + '.mp3', copy = buf.slice(0);
              Promise.resolve(st.upload(name, new Blob([copy], { type: 'audio/mpeg' })))
                .then(() => FS.set(FS.ref(FS.db, addr('voice', key)), { path: name, voice, text: String(text).slice(0, 300), at: Date.now(), metadata: { type: 'steady-voice-clip' } }))
                .catch(() => {});
            }
          }
        }
        if (buf) sendAudio(id, buf); else post('ttsAudio', { id, error: true });
      } catch (e) { post('ttsAudio', { id, error: true }); }
    };
    const post = (type, data) => { const w = frame.current && frame.current.contentWindow; if (w) w.postMessage({ steady: type, data }, '*'); };
    const onMsg = ev => {
      if (!frame.current || ev.source !== frame.current.contentWindow) return;
      const m = ev.data || {};
      if (!m.steady) return;
      if (m.steady === 'ready') { post('config', { canPublish: isOwner }); post('sessions', latest.sessions); if (latest.live) post('relay', latest.live); }
      if (m.steady === 'tts' && m.data) { tts(m.data); return; }
      if (m.steady === 'stt' && m.data) { stt(m.data); return; }
      if (m.steady === 'claude' && m.data) { claude(m.data); return; }
      if (m.steady === 'publish' && isOwner && m.data) { try { const p = FS.set(FS.ref(FS.db, addr('live')), m.data, false); if (p && p.catch) p.catch(() => {}); } catch (e) {} }
      if (m.steady === 'saveSession' && isOwner && m.data && m.data.id) {
        const now = Date.now();
        const rec = Object.assign({}, m.data, { metadata: { type: 'steady-session', createdAt: now, updatedAt: now } });
        FS.set(FS.ref(FS.db, addr('sessions', String(m.data.id).replace(/[.#$\[\]\/]/g, '-'))), rec).catch(() => {});
      }
    };
    window.addEventListener('message', onMsg);
    const offLive = FS.onValue(FS.ref(FS.db, addr('live')), s => { latest.live = s.val(); post('relay', latest.live); });
    const offSess = FS.onValue(FS.ref(FS.db, addr('sessions')), s => {
      const v = s.val() || {};
      latest.sessions = Object.keys(v).filter(k => k !== 'metadata').map(k => { const r = Object.assign({}, v[k]); delete r.metadata; return r; });
      post('sessions', latest.sessions);
    });
    const offVoice = FS.onValue(FS.ref(FS.db, addr('voice')), s => { latest.voice = s.val() || {}; });
    return () => { window.removeEventListener('message', onMsg); offLive(); offSess(); offVoice(); };
  }, [html]);

  return (
    <div className="w-full overflow-hidden rounded-2xl" style={{ height: 'var(--mana-app-height, 82vh)', background: '#AFC7EA' }}>
      {html ? (
        <iframe ref={frame} title="Steady" srcDoc={html} className="block w-full h-full border-0"
          allow="bluetooth; serial; microphone; autoplay; web-share; clipboard-write; fullscreen" />
      ) : (
        <div className="w-full h-full flex items-center justify-center text-[14px]" style={{ color: '#0B2540' }}>{err || 'Opening Steady…'}</div>
      )}
    </div>
  );
}
