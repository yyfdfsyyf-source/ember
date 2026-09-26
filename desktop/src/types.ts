/**
 * 界面层的共享类型。
 *
 * 会话 / 工作区 / 模型 / 命令的清单一律来自后端的 sessions / workspaces /
 * model_list / commands 事件（见 events/schema.ts），这里不再放任何静态样本，
 * 免得界面上出现一份和真数据长不一样的假列表。
 */

export type ScreenId = 'chat' | 'usage' | 'empty' | 'spec';
