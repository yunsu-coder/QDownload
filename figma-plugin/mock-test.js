// Minimal Figma Plugin API mock used to catch runtime errors in the builder.
const fs = require("fs");
const vm = require("vm");

class MockNode {
  constructor(type, name = "") {
    this.type = type;
    this.name = name;
    this.children = [];
    this.parent = null;
    this.x = 0;
    this.y = 0;
    this.width = 100;
    this.height = 100;
    this.fills = [];
    this.strokes = [];
    this.effects = [];
    this.reactions = [];
    this.characters = "";
  }
  resize(w, h) {
    this.width = w;
    this.height = h;
  }
  resizeWithoutConstraints(w, h) {
    this.resize(w, h);
  }
  appendChild(node) {
    if (node.parent) {
      node.parent.children = node.parent.children.filter(n => n !== node);
    }
    node.parent = this;
    this.children.push(node);
  }
  remove() {
    if (this.parent) {
      this.parent.children = this.parent.children.filter(n => n !== this);
    }
    this.parent = null;
  }
  findOne(fn) {
    for (const child of this.children) {
      if (fn(child)) return child;
      const nested = child.findOne(fn);
      if (nested) return nested;
    }
    return null;
  }
  setBoundVariable() {}
  addComponentProperty() {
    return `prop#${Math.random().toString(36).slice(2)}`;
  }
  createInstance() {
    return new MockNode("INSTANCE", `${this.name} / Instance`);
  }
  setProperties(props) {
    this.properties = { ...(this.properties || {}), ...props };
  }
  clone() {
    const n = new MockNode(this.type, this.name);
    Object.assign(n, this);
    n.children = this.children.map(child => {
      const copy = child.clone();
      copy.parent = n;
      return copy;
    });
    n.parent = null;
    n.reactions = [];
    return n;
  }
}

const page = new MockNode("PAGE", "Page 1");
const mockVariables = [];

const figma = {
  currentPage: page,
  root: { children: [page] },
  viewport: { scrollAndZoomIntoView() {} },
  notify() {},
  closePlugin(message) {
    console.log(message);
  },
  async loadFontAsync() {},
  async listAvailableFontsAsync() {
    return [];
  },
  createFrame() {
    return new MockNode("FRAME");
  },
  createComponent() {
    return new MockNode("COMPONENT");
  },
  createText() {
    return new MockNode("TEXT");
  },
  createEllipse() {
    return new MockNode("ELLIPSE");
  },
  createNodeFromSvg() {
    return new MockNode("FRAME", "SVG");
  },
  createImage() {
    return { hash: "mock-image-hash" };
  },
  base64Decode(s) {
    return Buffer.from(s, "base64");
  },
  async getLocalTextStylesAsync() {
    return [];
  },
  createTextStyle() {
    return { id: `text-style-${Math.random()}`, name: "" };
  },
  async getLocalEffectStylesAsync() {
    return [];
  },
  createEffectStyle() {
    return { id: `effect-style-${Math.random()}`, name: "", effects: [] };
  },
  variables: {
    async getLocalVariableCollectionsAsync() {
      return [];
    },
    async getLocalVariablesAsync() {
      return mockVariables;
    },
    createVariableCollection(name) {
      return {
        id: `collection-${Math.random()}`,
        name,
        modes: [{ modeId: "mode-1", name: "Mode 1" }],
        variableIds: [],
        renameMode(modeId, newName) {
          const mode = this.modes.find(m => m.modeId === modeId);
          if (mode) mode.name = newName;
        }
      };
    },
    createVariable(name, collection, type) {
      const v = {
        id: `variable-${mockVariables.length}`,
        name,
        type,
        scopes: [],
        setValueForMode() {},
        setVariableCodeSyntax() {}
      };
      mockVariables.push(v);
      collection.variableIds.push(v.id);
      return v;
    },
    setBoundVariableForPaint(paint) {
      return paint;
    }
  }
};

global.figma = figma;

let code = fs.readFileSync("figma-plugin/code.js", "utf8");
code = code.replace(/build\(\)\.catch\(err => \{[\s\S]*$/m, "");
vm.runInThisContext(`${code}\nglobalThis.__quickdlBuild = build;`);

(async () => {
  await globalThis.__quickdlBuild();
  const top = page.children.map(n => n.name);
  if (!top.includes("QuickDL / 01 Downloading")) throw new Error("main frame missing");
  if (!top.includes("QuickDL / 02 Paused")) throw new Error("paused frame missing");
  if (!top.includes("QuickDL / 03 New Download")) throw new Error("modal frame missing");
  console.log("mock_build_ok", top.join(" | "));
})().catch(err => {
  console.error(err);
  process.exit(1);
});
