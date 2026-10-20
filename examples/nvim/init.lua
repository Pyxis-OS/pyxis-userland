-- An example Neovim configuration for Pyxis. Nothing loads it automatically.
-- To try it, copy it to where Neovim looks for your configuration:
--   mkdir home://.config
--   mkdir home://.config/nvim
--   cp boot://share/examples/nvim/init.lua home://.config/nvim/init.lua
--
-- Tree-sitter highlighting is already on for C, Lua, Vim, help, query and
-- Markdown, and .h files are treated as C. This file only adds clangd.

-- Start clangd for C and C++ buffers. It comes from the clang-hosted package:
-- configure a repository, then run: pkg install clang-hosted
vim.lsp.enable('clangd')

-- When a server attaches, complete as you type if it supports completion.
vim.api.nvim_create_autocmd('LspAttach', {
  callback = function(args)
    local client = vim.lsp.get_client_by_id(args.data.client_id)
    if client and client:supports_method('textDocument/completion') then
      vim.lsp.completion.enable(true, client.id, args.buf, { autotrigger = true })
    end
  end,
})

-- Ctrl-Space opens the completion menu by hand in insert mode.
vim.keymap.set('i', '<C-Space>', function()
  vim.lsp.completion.get()
end)
