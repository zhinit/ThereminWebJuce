import { useState, useRef } from "react";
import "./App.css";

function App() {
  const [inLoop, setInLoop] = useState(false);
  const [playbackReady, setPlaybackReady] = useState(false);
  const [ottAmount, setOttAmount] = useState(0);
  const [distortionAmount, setDistortionAmount] = useState(0);
  const [reverbAmount, setReverbAmount] = useState(0.3);
  const audioContextRef = useRef<AudioContext | null>(null);
  const workletNodeRef = useRef<AudioWorkletNode | null>(null);

  const initAudio = async () => {
    if (audioContextRef.current) return;

    const ctx = new AudioContext();

    // Fetch the Emscripten glue code in main thread
    const response = await fetch("/audio-engine.js");
    const scriptCode = await response.text();

    await ctx.audioWorklet.addModule("/dsp-processor.js");

    const node = new AudioWorkletNode(ctx, "dsp-processor", {
      outputChannelCount: [2],
    });
    node.connect(ctx.destination);

    node.port.onmessage = async (e) => {
      if (e.data.type === "ready") {
        await loadIR();
        await loadSample();
        setPlaybackReady(true);
      }
    };

    const loadIR = async () => {
      const irFile = await fetch("/ir.wav");
      const arrayBuffer = await irFile.arrayBuffer();
      const audioBuffer = await ctx.decodeAudioData(arrayBuffer);

      const numChannels = audioBuffer.numberOfChannels;
      const length = audioBuffer.length;
      const irSamples = new Float32Array(length * numChannels);

      // Interleave channels for stereo IR
      for (let ch = 0; ch < numChannels; ch++) {
        const channelData = audioBuffer.getChannelData(ch);
        for (let i = 0; i < length; i++) {
          irSamples[i * numChannels + ch] = channelData[i];
        }
      }

      node.port.postMessage({ type: "loadIR", irSamples, irLength: length, numChannels });
    };

    const loadSample = async () => {
      const kickFile = await fetch("/kick.wav");
      const arrayBuffer = await kickFile.arrayBuffer();
      const audioBuffer = await ctx.decodeAudioData(arrayBuffer);
      const samples = audioBuffer.getChannelData(0);
      node.port.postMessage({ type: "loadSample", samples })
    }

    // Send the script code to the worklet
    node.port.postMessage({ type: "init", scriptCode });

    audioContextRef.current = ctx;
    workletNodeRef.current = node;
  };

  const handleCue = async () => {
    if (!audioContextRef.current) {
      await initAudio();
    }

    if (audioContextRef.current?.state === "suspended") {
      await audioContextRef.current.resume();
    }

    workletNodeRef.current?.port.postMessage({ type: "play" });
  };

  const handlePlayPauseButton = async () => {
    // First click just initializes, second click starts the loop
    if (!playbackReady) {
      await initAudio();
      return;
    }

    if (audioContextRef.current?.state === "suspended") {
      await audioContextRef.current.resume();
    }

    const inLoopNew = !inLoop;
    setInLoop(inLoopNew);
    workletNodeRef.current?.port.postMessage({ type: "loop", inLoop: inLoopNew })
  }

  const handleOTTAmount = (e: React.ChangeEvent<HTMLInputElement>) => {
    const amount = parseFloat(e.target.value);
    setOttAmount(amount);
    workletNodeRef.current?.port.postMessage({ type: "ottAmount", amount });
  }

  const handleDistortionAmount = (e: React.ChangeEvent<HTMLInputElement>) => {
    const amount = parseFloat(e.target.value);
    setDistortionAmount(amount);
    // Map 0-1 to drive 1-20 (drive=1 is nearly clean, drive=20 is heavy)
    const drive = 1.0 + amount * 19.0;
    workletNodeRef.current?.port.postMessage({ type: "distortionAmount", drive });
  }

  const handleReverbAmount = (e: React.ChangeEvent<HTMLInputElement>) => {
    const amount = parseFloat(e.target.value);
    setReverbAmount(amount);
    workletNodeRef.current?.port.postMessage({ type: "reverbMix", wet: amount, dry: 1 - amount });
  }

  return (
    <div>
      <h1>C++ JUCE WASM Sampler POC</h1>
      <button
        onPointerDown={handleCue}
      >
        Cue
      </button>
      <button
        onPointerDown={handlePlayPauseButton}
      >
        Play/Pause
      </button>
      <div>
        <label>Distortion: {distortionAmount.toFixed(2)}</label>
        <input
          type="range"
          min="0"
          max="1"
          step="0.01"
          value={distortionAmount}
          onChange={handleDistortionAmount}
        />
      </div>
      <div>
        <label>OTT Amount: {ottAmount.toFixed(2)}</label>
        <input
          type="range"
          min="0"
          max="1"
          step="0.01"
          value={ottAmount}
          onChange={handleOTTAmount}
        />
      </div>
      <div>
        <label>Reverb: {reverbAmount.toFixed(2)}</label>
        <input
          type="range"
          min="0"
          max="1"
          step="0.01"
          value={reverbAmount}
          onChange={handleReverbAmount}
        />
      </div>
    </div>
  );
}

export default App;
