﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using UnityEngine.UI;
using UnityEngine;
using GameLogic;
using U3D_Render.USpeedUI.UWidgets.UControls.UListView;
using DataCenter;
using UIWidgets;
using DG.Tweening;
using USpeedUI.Social;

namespace USpeedUI.ClanDonate
{
    public class ClanDonateWnd : UIBaseSubWnd<ClanDonateWndView>
    {
        // 获取ID
        public override int GetWndType()
        {
            return (int)ClanListFrameWndType.WND_CLAN_DONATE;
        }

        // 获取资源路径
        public override String GetPrefabPath()
        {
            return "Social/UViewClanDonate";
        }

        // 初始化
        public override bool Init()
        {
            base.Init();

            return true;
        }

        // 设置是否可见
        public override void SetVisible(bool visible)
        {
            if (visible)
            {
                if (EntityFactory.MainHeroView == null)
                    return;

                int nClanID = EntityFactory.MainHeroView.Property.GetNumProp(ENTITY_PROPERTY.PROPERTY_CLAN);
                if (nClanID == 0)
                {
                    UIUtil.ShowSystemMessage(EMChatTipID.CHAT_TIP_CLAN_OPENWND_NO_CLAN);
                    return;
                }

                ViewEventHelper.Instance.SendCommand(GameLogicDef.GVIEWCMD_CLAN_REQUEST_CARDLIST);
            }
            base.SetVisible(visible);
        }

        // 接受消息
        public override void OnMessage(WndMsgID msgID, UIMsgData msgData)
        {
            UIMsgCmdData data = msgData as UIMsgCmdData;
            switch (msgID)
            {
                case WndMsgID.WND_MSG_CLAN_REQUEST_CARDLIST:
                    {
                        if (m_wndView != null)
                        {
                            m_wndView.OnClanUpdateRequestCard();
                            LogicDataCenter.clanDataManager.ClearRequestcardTipCount();
                        }
                    }
                    break;

                case WndMsgID.WND_MSG_CLAN_REQUEST_CARDLIST_INFO:
                    {
                        if (m_wndView != null)
                        {
                            m_wndView.OnClanUpdateRequestCardInfo();
                        }
                    }
                    break;

                case WndMsgID.WND_MSG_CLAN_UPDATE_CARDLIST:
                    {
                        if (m_wndView != null)
                            m_wndView.OnClanUpdateRequestCard();
                    }
                    break;

                case WndMsgID.WND_MSG_CLAN_UPDATE_DONATELOG:
                    {
                        if (m_wndView != null)
                            m_wndView.OnClanUpdateDonateLog();
                    }
                    break;
                    
                default:
                    break;
            }
        }
    }

    public class ClanDonateWndView : UIBaseSubWndView
    {
        public UTileClanCard tileClanCard;
        public Text[] cardInfoDesc;
        public ListView logList;
        public Toggle selfLogToggle;
        public Text selfLogText;
        public Button requestBtn;
        public Text requestBtnText;
        public Image requestBtnEffect;
        private Sequence requestBtnEffectSequence;
        public Text cooldownTimeText;
        public Text requestLimitText;

        public override bool Init(IUISubWnd wnd)
        {
            for (int i = 0; i < cardInfoDesc.Length; ++i)
            {
                cardInfoDesc[i].text = ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "CardInfoDesc" + (i + 1));
            }
            requestBtn.GetComponent<UTooltipTrigger>().SetText(UTooltipParamName.BodyText, ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "HeroStarTooltip"));
            selfLogText.text = ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "SelfLogDesc");
            requestBtnText.text = ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "RequestCardBtn");
            cooldownTimeText.text = "";
            requestBtnEffect.color = Color.clear;

            return base.Init(wnd);
        }

        public override void SetVisible(bool visible)
        {
            base.SetVisible(visible);

            if (!visible)
            {
                if (requestBtnEffectSequence != null)
                    requestBtnEffectSequence.Kill(true);
            }
            else
            {
                requestBtnEffect.color = Color.clear;
            }
        }

        public void OnJoinWarBtnClick()
        {
            m_wnd.SetVisible(false);
            UISystem.Instance.SendWndMessage(WndMsgID.WND_MSG_WAR_MAIN_OPEN, null);
        }

        public void OnSelfLogSetChanged(bool bSelfLog)
        {
            OnClanUpdateDonateLog();
        }

        public void OnRequestSelfCardBtnClick()
        {
            UISystem.Instance.ShowWnd(WndID.WND_ID_CLAN_SELECTREQUESTCARD, true);
        }

        public void OnClanUpdateRequestCard()
        {
            tileClanCard.DataSource.BeginUpdate();

            tileClanCard.DataSource.Clear();

            List<cmd_clan_request_cardlist> cardList = LogicDataCenter.clanDataManager.CardListData;
            for (int i = 0; i < cardList.Count; ++i)
            {
                UTileItemClanCard item = new UTileItemClanCard();
                item.data = cardList[i];

                tileClanCard.DataSource.Add(item);
            }

            tileClanCard.DataSource.EndUpdate();
        }

        public void OnClanUpdateRequestCardInfo()
        {
            int nLeftCooldownTime = LogicDataCenter.clanDataManager.LeftRequestCooldownTime;
            if (nLeftCooldownTime <= 0)
            {
                cooldownTimeText.text = "";
            }
            else
            {
                TimeSpan timeSpan = TimeSpan.FromSeconds(nLeftCooldownTime);
                cooldownTimeText.text = String.Format("{0} : {1:D2}:{2:D2}:{3:D2}", ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "CooldownDesc"), timeSpan.Hours, timeSpan.Minutes, timeSpan.Seconds);
            }

            if (LogicDataCenter.clanDataManager.LeftRequestNum > 0)
            {
                requestLimitText.text = "";
            }
            else
            {
                requestLimitText.text = ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "RequestLimitDesc");
                // 到达日上限，冷却提示直接隐藏
                cooldownTimeText.text = "";
            }

            if (nLeftCooldownTime > 0 || LogicDataCenter.clanDataManager.LeftRequestNum == 0)
            {
                requestBtn.interactable = false;

                if (requestBtnEffectSequence != null)
                    requestBtnEffectSequence.Kill(true);
                requestBtnEffect.color = Color.clear;
            }
            else
            {
                requestBtn.interactable = true;

                if (requestBtnEffectSequence == null)
                {
                    Color effectBaseColor = Color.white;
                    effectBaseColor.a = 0f;
                    requestBtnEffect.color = effectBaseColor;
                    requestBtnEffectSequence = DOTween.Sequence();
                    requestBtnEffectSequence.Append(DOTween.ToAlpha(() => requestBtnEffect.color, x => requestBtnEffect.color = x, 1f, 0.5f));
                    requestBtnEffectSequence.Append(DOTween.ToAlpha(() => requestBtnEffect.color, x => requestBtnEffect.color = x, 0f, 0.5f).SetDelay(0.2f));
                    requestBtnEffectSequence.SetLoops(-1, LoopType.Restart);
                    requestBtnEffectSequence.OnKill(() => requestBtnEffectSequence = null);
                }
            }
        }

        public void OnClanUpdateDonateLog()
        {
            bool bSelfOnly = selfLogToggle.isOn;
            int nSelfActorID = 0;
            if (EntityFactory.MainHeroView != null)
                nSelfActorID = EntityFactory.MainHeroView.Property.GetNumProp(ENTITY_PROPERTY.PROPERTY_ID);
            logList.DataSource.BeginUpdate();

            logList.DataSource.Clear();
            List<cmd_clan_update_donatelog> donateLog = LogicDataCenter.clanDataManager.DonateLog;
            for (int i = 0; i < donateLog.Count; ++i)
            {
                cmd_clan_update_donatelog curLog = donateLog[i];
                if (bSelfOnly && !(curLog.nGaveActorID == nSelfActorID || curLog.nTakeActorID == nSelfActorID))
                    continue;
                String desc1 = ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "DonateLogDesc1");
                String desc2 = ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "DonateLogDesc2");
                String desc3 = ULocalizationService.Instance.Get("UIView", "ClanRequestCard", "DonateLogDesc3");
                String strLog = String.Format("<color=#ffffffff>{0}</color> <color=#ffffffff>{1}</color> <color=#ffffffff>{2}</color> <color=#60e333ff>{3}{4}{5}{6}</color>", curLog.szGaveActorName, desc1, curLog.szTakeActorName, curLog.nCardNum, desc2, curLog.szCardName, desc3);

                logList.DataSource.Add(strLog);
            }

            logList.DataSource.EndUpdate();
        }

        public void AutoSetting(bool _bToggle)
        {
            //ImageSetting.currAsinoEnabled = _bToggle ? 1 : 0;
            //ImageSetting.SetTextureAniso(ImageSetting.currAsinoEnabled);

            OnClanUpdateDonateLog();
        }
    }
}
