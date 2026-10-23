const path = require('node:path');
const {existsSync, statSync} = require('node:fs');

// run.mjs stages this file in temp/docusaurus, where its dependencies live.
module.exports = async function createConfig() {
  const projectDir = process.env.OPTO_PROJECT_ROOT;
  if (!projectDir) throw new Error('请通过 06_website 的 npm 脚本运行文档站。');
  const {default: remarkMath} = await import('remark-math');
  const {default: rehypeKatex} = await import('rehype-katex');
  const docsDir = path.join(projectDir, '01_docs');

  function isProjectAttachment(target) {
    const relative = path.relative(projectDir, target);
    return relative && !relative.startsWith('..') && !path.isAbsolute(relative)
      && !target.startsWith(`${docsDir}${path.sep}`)
      && existsSync(target) && statSync(target).isFile();
  }

  // Docusaurus assumes .md links are docs and extensionless links are routes.
  // Keep repository-relative source links and bundle only actual attachments.
  function repositoryAttachments() {
    return (tree, file) => {
      function walk(node) {
        if ((node.type === 'link' || node.type === 'definition')
          && node.url && !/^(?:[a-z][a-z\d+.-]*:|\/|#)/i.test(node.url)) {
          const [, pathname, suffix] = node.url.match(/^([^?#]*)(.*)$/s);
          const target = path.resolve(path.dirname(file.path), decodeURIComponent(pathname));
          if ((!path.extname(target) || /\.mdx?$/i.test(target)) && isProjectAttachment(target)) {
            node.url = `@site/${path.relative(__dirname, target).split(path.sep).join('/')}${suffix}`;
          }
        }
        node.children?.forEach(walk);
      }
      walk(tree);
    };
  }

  function checkMarkdownLink({sourceFilePath, url}) {
    if (url.startsWith('@site/') && isProjectAttachment(path.resolve(__dirname, url.slice(6).split(/[?#]/)[0]))) {
      return url;
    }
    throw new Error(`文档链接无效：${sourceFilePath} → ${url}`);
  }

  return {
    title: '光电象棋机器人 2027',
    tagline: '计划、方案与实验记录',
    url: 'http://localhost',
    baseUrl: '/',
    onBrokenLinks: 'throw',
    i18n: {defaultLocale: 'zh-Hans', locales: ['zh-Hans']},
    staticDirectories: [],
    markdown: {
      format: 'detect',
      hooks: {onBrokenMarkdownLinks: checkMarkdownLink, onBrokenMarkdownImages: 'throw'},
    },
    presets: [
      ['classic', {
        docs: {
          path: docsDir,
          routeBasePath: '/',
          sidebarPath: path.join(__dirname, 'sidebars.js'),
          beforeDefaultRemarkPlugins: [repositoryAttachments],
          remarkPlugins: [remarkMath],
          rehypePlugins: [rehypeKatex],
          showLastUpdateTime: false,
        },
        blog: false,
        pages: false,
        theme: {customCss: [require.resolve('katex/dist/katex.min.css'), path.join(__dirname, 'custom.css')]},
      }],
    ],
    themeConfig: {
      navbar: {
        title: '光电象棋机器人',
        items: [{type: 'docSidebar', sidebarId: 'project', label: '项目文档', position: 'left'}],
      },
      docs: {sidebar: {hideable: true, autoCollapseCategories: false}},
      colorMode: {defaultMode: 'light', respectPrefersColorScheme: true},
      prism: {additionalLanguages: ['python', 'powershell', 'bash', 'json', 'c', 'cpp']},
      footer: {style: 'light', copyright: '2027 光电象棋机器人项目'},
    },
  };
};
