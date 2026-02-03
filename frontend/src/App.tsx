import { useState, useRef } from "react";
import "./App.css";

function App() {
  // const [isPlaying, setIsPlaying] = useState(false);
  const [isReady, setIsReady] = useState(false);
  const audioContextRef = useRef<AudioContext | null>(null);
  const workletNodeRef = useRef<AudioWorkletNode | null>(null);

  const initAudio = async () => {
    if (audioContextRef.current) return;

    const ctx = new AudioContext();

    // Fetch the Emscripten glue code in main thread
    const response = await fetch("/audio-engine.js");
    const scriptCode = await response.text();

    await ctx.audioWorklet.addModule("/dsp-processor.js");

    const node = new AudioWorkletNode(ctx, "dsp-processor");
    node.connect(ctx.destination);

    node.port.onmessage = (e) => {
      if (e.data.type === "ready") {
        setIsReady(true);
        loadSample();
      }
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

  const triggerSample = async () => {
    if (!audioContextRef.current) {
      await initAudio();
    }

    if (audioContextRef.current?.state === "suspended") {
      await audioContextRef.current.resume();
    }

    workletNodeRef.current?.port.postMessage({ type: "play" });
  };

  return (
    <div>
      <h1>C++ JUCE WASM Sampler POC</h1>
      <button
        onPointerDown={triggerSample}
      >
        Press Me!
      </button>
      {!isReady && audioContextRef.current && <p>Loading WASM...</p>}
    </div>
  );
}

export default App;
