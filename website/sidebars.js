/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  docs: [
    {
      type: 'category',
      label: 'Start here',
      collapsed: false,
      items: ['intro', 'getting-started', 'dependencies', 'packaging'],
    },
    {
      type: 'category',
      label: 'Core runtime',
      collapsed: false,
      items: ['agent-runtime', 'orchestration', 'reasoning', 'planning', 'multi-agent', 'reflection', 'learning-adaptation', 'exploration-discovery'],
    },
    {
      type: 'category',
      label: 'Models and tools',
      items: ['llm-providers', 'resource-routing', 'llm-streaming', 'llm-tools', 'http-backends'],
    },
    {
      type: 'category',
      label: 'State and knowledge',
      items: ['memory-management', 'memory-deployment', 'knowledge-retrieval'],
    },
    {
      type: 'category',
      label: 'Model Context Protocol',
      items: ['mcp', 'mcp-host-compatibility'],
    },
    {
      type: 'category',
      label: 'Agent interoperability',
      items: ['a2a'],
    },
    {
      type: 'category',
      label: 'Controlled execution',
      items: ['execution-runtime'],
    },
    {
      type: 'category',
      label: 'Operations and governance',
      items: ['security-governance', 'guardrails', 'evaluation', 'observability'],
    },
  ],
};

export default sidebars;
