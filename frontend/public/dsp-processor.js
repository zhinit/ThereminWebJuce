class DSPProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.engine = null;
    this.module = null;
    this.heapBuffer = null;
    this.port.onmessage = (e) => this.handleMessage(e.data);
  }

  async handleMessage(data) {
    // initialization - receive script from main thread
    if (data.type === "init") {
      // Execute the Emscripten glue code sent from main thread
      const fn = new Function(data.scriptCode + "; return createAudioEngine;");
      const createAudioEngine = fn();
      const module = await createAudioEngine();
      this.engine = new module.SineOscillator();
      this.engine.setSampleRate(sampleRate);
      this.module = module;
      this.port.postMessage({ type: "ready" });
    }
    // when ui sends play msg
    if (data.type === "play") {
      this.engine?.setPlaying(true);
    }
    // when ui sends stop msg
    if (data.type === "stop") {
      this.engine?.setPlaying(false);
    }
  }

  process(inputs, outputs, parameters) {
    // if engine or module missing, return but keep processor alive
    if (!this.engine || !this.module) return true;

    // set browser output first output bus, first channel on that bus (mono)
    const output = outputs[0][0];
    // set numSamples to size of browser audio buffer
    const numSamples = output.length;

    // if buffer has not been allocated or is too small
    if (!this.heapBuffer || this.heapBuffer.length < numSamples) {
      if (this.heapBuffer) this.module._free(this.heapBuffer);
      this.heapBuffer = this.module._malloc(numSamples * 4);
    }

    // call wasm process function which puts result in wasm heap memory
    this.engine.process(this.heapBuffer, numSamples);

    // pull audio from wasm heap memory so it can be played in browser
    const wasmOutput = new Float32Array(
      this.module.HEAPF32.buffer,
      this.heapBuffer,
      numSamples
    );
    output.set(wasmOutput);

    // call me again when the next block of samples is needed
    return true;
  }
}

registerProcessor("dsp-processor", DSPProcessor);
