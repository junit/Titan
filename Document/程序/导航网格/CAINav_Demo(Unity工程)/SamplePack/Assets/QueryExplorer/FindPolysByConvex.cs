/*
 * Copyright (c) 2011-2012 Stephen A. Pratt
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
using UnityEngine;
using org.critterai.nav.u3d;
using org.critterai.u3d;
using org.critterai.nav;

namespace org.critterai.samples.qe
{
    public sealed class FindPolysByConvex
        : IQueryExplorerState 
    {
        private NavGroup mHelper;

        private float mYOffset = 0;
        private Vector3[] mBasePoly = new Vector3[6]
        {
            new Vector3(1f, 0.75f, 3.4f)
            , new Vector3(2.4f, 0.25f, 2.4f)
            , new Vector3(3.6f , 0.25f, -2f)
            , new Vector3(-1.6f, 0, -2.6f)
            , new Vector3(-2.4f, 0.25f, -2f)
            , new Vector3(-3.4f, 0, 2f)
        };

        private Vector3[] mSearchPoly = new Vector3[6];

        private NavmeshPoint mPosition;
        private bool mHasPosition = false;
        private string mMessage;

        private BufferHandler<uint> mPolyRefs;
        private uint[] mParentRefs;
        private float[] mCosts;
        private Vector3[] mCentroids;
        private int mResultCount;

        public QEStateType StateType
        {
            get { return QEStateType.FindPolysByConvex; }
        }

        public void Enter(NavGroup helper)
        {
            mHelper = helper;
            SimGUIUtil.BuildLabelRegion(false);

            mPolyRefs = new BufferHandler<uint>("Search Buffer Size"
                , 1, QEUtil.SmallBufferInit, 1, QEUtil.SmallBufferMax);

            int size = mPolyRefs.MaxElementCount;
            mParentRefs = new uint[size];
            mCosts = new float[size];
            mCentroids = new Vector3[size];
            mResultCount = 0;
            mHasPosition = false;

            SimGUIUtil.contextHelpText = string.Format("Translate Y: [{0}] [{1}]"
                , StdButtons.AdjustYMinus, StdButtons.AdjustYPlus);

            SimGUIUtil.contextControlZone.height = SimGUIUtil.LineHeight * 2.5f;
            SimGUIUtil.contextActive = true;
        }

        public void Exit()
        {
            mPolyRefs = null;
            mParentRefs = null;
            mCosts = null;
            mCentroids = null;

            SimGUIUtil.labels.Clear();
            SimGUIUtil.contextHelpText = "";
            SimGUIUtil.contextActive = false;
        }

        public void Update()
        {
            // Must set message before returning.

            mHasPosition = false;
            mResultCount = 0;

            mYOffset += QEUtil.GetYFactor();

            Vector3 trash;
            QEUtil.SearchResult result =
                QEUtil.HandleStandardPolySearch(mHelper, out trash, out mPosition, out mMessage);

            mHasPosition = (result & QEUtil.SearchResult.HitNavmesh) != 0;

            if (!mHasPosition) 
                return;

            Vector3 pos = mPosition.point;
            for (int i = 0; i < mSearchPoly.Length; i++)
            {
                mSearchPoly[i] = pos + mBasePoly[i];
                mSearchPoly[i].y += mYOffset;
            }

            if (mPolyRefs.HandleResize())
            {
                int size = mPolyRefs.MaxElementCount;
                mParentRefs = new uint[size];
                mCosts = new float[size];
                mCentroids = new Vector3[size];
            }

            NavStatus status = mHelper.query.FindPolys(mPosition.polyRef, mSearchPoly
                , mHelper.filter
                , mPolyRefs.buffer, mParentRefs, mCosts, out mResultCount);

            mMessage = "FindPolys: " + status.ToString() + ".";

            if (mResultCount > 0)
                NavDebug.GetCentroids(mHelper.mesh, mPolyRefs.buffer, mResultCount, mCentroids);
        }

        public void OnGUI()
        {
            if (mResultCount > 0)
            {
                Camera cam = Camera.main;

                for (int i = 0; i < mResultCount; i++)
                {
                    if (mParentRefs[i] == 0)
                        continue;

                    Vector3 center = GetBufferedCentroid(mPolyRefs.buffer[i]);

                    center = cam.WorldToScreenPoint(center);
                    center.y = cam.pixelHeight - center.y + 4; 

                    GUI.Label(new Rect(center.x, center.y, 100, SimGUIUtil.LineHeight)
                        , string.Format("{0:F2}", mCosts[i]));
                }
            }

            GUILayout.BeginArea(SimGUIUtil.contextControlZone, GUI.skin.box);
            mPolyRefs.OnGUI();
            GUILayout.EndArea();

            LabelRegion mLabels = SimGUIUtil.labels;

            mLabels.Clear();

            const int LabelCount = 1;
            int slotIndex = mLabels.SlotCount - 1 - LabelCount;

            mLabels.Set(slotIndex++, "Polygons found", mResultCount.ToString());

            mLabels.Last = mMessage;
        }

        public void OnRenderObject()
        {
            NavDebug.Draw(mHelper.mesh, mPolyRefs.buffer, mResultCount);

            Color c = QEUtil.SelectColor;
            c.a = 0.25f;

            if (mHasPosition)
                DebugDraw.ConvexPoly(mSearchPoly, mSearchPoly.Length, c);

            if (mResultCount > 0)
            {
                for (int i = 0; i < mResultCount; i++)
                {
                    if (mParentRefs[i] == 0)
                        continue;

                    Vector3 center = GetBufferedCentroid(mPolyRefs.buffer[i]);
                    Vector3 pcenter;

                    if (mParentRefs[i] == mPosition.polyRef)
                        pcenter = mPosition.point;
                    else
                        pcenter = GetBufferedCentroid(mParentRefs[i]);

                    DebugDraw.Arrow(pcenter, center, 0, QEUtil.HeadScaleSm, NavDebug.goalColor);
                }
            }
        }

        private Vector3 GetBufferedCentroid(uint polyRef)
        {
            for (int i = 0; i < mResultCount; i++)
            {
                if (mPolyRefs.buffer[i] == polyRef)
                    return mCentroids[i];
            }
            return Vector3.zero;
        }
    }
}
