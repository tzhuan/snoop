import { contextBridge, ipcRenderer } from 'electron'

contextBridge.exposeInMainWorld('dialogAPI', {
  submit: (data) => ipcRenderer.send('dialog-submit', data),
  cancel: () => ipcRenderer.send('dialog-cancel'),
})
