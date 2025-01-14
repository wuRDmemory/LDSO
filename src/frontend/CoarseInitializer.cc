#include "internal/FrameHessian.h"
#include "internal/GlobalCalib.h"
#include "internal/GlobalFuncs.h"
#include "frontend/PixelSelector2.h"
#include "frontend/CoarseInitializer.h"
#include "frontend/nanoflann.h"
#include <iostream>

namespace ldso {

    CoarseInitializer::CoarseInitializer(int ww, int hh) : thisToNext_aff(0, 0), thisToNext(SE3()) {
        for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
            points[lvl] = 0;
            numPoints[lvl] = 0;
        }

        JbBuffer = new Vec10f[ww * hh];
        JbBuffer_new = new Vec10f[ww * hh];

        fixAffine = true;
        printDebug = false;

        wM.diagonal()[0] = wM.diagonal()[1] = wM.diagonal()[2] = SCALE_XI_ROT;
        wM.diagonal()[3] = wM.diagonal()[4] = wM.diagonal()[5] = SCALE_XI_TRANS;
        wM.diagonal()[6] = SCALE_A;
        wM.diagonal()[7] = SCALE_B;
    }

    CoarseInitializer::~CoarseInitializer() {
        for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
            if (points[lvl] != 0) delete[] points[lvl];
        }

        delete[] JbBuffer;
        delete[] JbBuffer_new;
    }

    bool CoarseInitializer::trackFrame(shared_ptr<FrameHessian> newFrameHessian) {

        newFrame = newFrameHessian;
        int maxIterations[] = {5, 5, 10, 30, 50};

        alphaK = 2.5 * 2.5;
        alphaW = 150 * 150;
        regWeight = 0.8;
        couplingWeight = 1;

        if (!snapped) {
            // 设置translation为0
            thisToNext.translation().setZero();
            for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
                int npts = numPoints[lvl];
                Pnt *ptsl = points[lvl];
                for (int i = 0; i < npts; i++) {
                    ptsl[i].iR = 1;
                    ptsl[i].idepth_new = 1;
                    ptsl[i].lastHessian = 0;
                }
            }
        }

        SE3 refToNew_current = thisToNext;
        AffLight refToNew_aff_current = thisToNext_aff;

        // 如果曝光实践都是大于0的
        if (firstFrame->ab_exposure > 0 && newFrame->ab_exposure > 0)
            // 优化曝光参数
            refToNew_aff_current = AffLight(logf(newFrame->ab_exposure / firstFrame->ab_exposure),
                                            0); // coarse approximation.

        // 从金字塔顶层到底层
        Vec3f latestRes = Vec3f::Zero();
        for (int lvl = pyrLevelsUsed - 1; lvl >= 0; lvl--) {

            // 当金字塔不在最顶层的时候，进行向下传播
            if (lvl < pyrLevelsUsed - 1)
                // 如果不在金字塔的顶层，则进行参数的向下传播
                propagateDown(lvl + 1);

            Mat88f H, Hsc;
            Vec8f b, bsc;
            // 这个resetPoints的意义待定
            resetPoints(lvl);

            // 
            Vec3f resOld = calcResAndGS(lvl, H, b, Hsc, bsc, refToNew_current, refToNew_aff_current, false);

            // 
            applyStep(lvl);

            float lambda = 0.1;
            float eps = 1e-4;
            int fails = 0;

            // L-M 方法
            int iteration = 0;
            while (true) {
                Mat88f Hl = H;
                for (int i = 0; i < 8; i++) Hl(i, i) *= (1 + lambda);

                // schur补
                Hl -= Hsc * (1 / (1 + lambda));
                Vec8f bl = b - bsc * (1 / (1 + lambda));

                Hl = wM * Hl * wM * (0.01f / (w[lvl] * h[lvl]));
                bl = wM * bl * (0.01f / (w[lvl] * h[lvl]));

                // 
                Vec8f inc;
                if (fixAffine) {
                    inc.head<6>() = -(wM.toDenseMatrix().topLeftCorner<6, 6>() *
                                      (Hl.topLeftCorner<6, 6>().ldlt().solve(bl.head<6>())));
                    inc.tail<2>().setZero();
                } else
                    inc = -(wM * (Hl.ldlt().solve(bl)));    //=-H^-1 * b.


                SE3 refToNew_new = SE3::exp(inc.head<6>().cast<double>()) * refToNew_current;
                AffLight refToNew_aff_new = refToNew_aff_current;
                refToNew_aff_new.a += inc[6];
                refToNew_aff_new.b += inc[7];
                // 计算新的逆深度
                doStep(lvl, lambda, inc);


                Mat88f H_new, Hsc_new;
                Vec8f b_new, bsc_new;

                // 
                Vec3f resNew = calcResAndGS(lvl, H_new, b_new, Hsc_new, bsc_new, refToNew_new, refToNew_aff_new, false);
                Vec3f regEnergy = calcEC(lvl);

                float eTotalNew = (resNew[0] + resNew[1] + regEnergy[1]);
                float eTotalOld = (resOld[0] + resOld[1] + regEnergy[0]);


                bool accept = eTotalOld > eTotalNew;

                if (accept) {

                    if (resNew[1] == alphaK * numPoints[lvl])
                        snapped = true;
                    H = H_new;
                    b = b_new;
                    Hsc = Hsc_new;
                    bsc = bsc_new;
                    resOld = resNew;
                    refToNew_aff_current = refToNew_aff_new;
                    refToNew_current = refToNew_new;
                    applyStep(lvl);
                    optReg(lvl);
                    lambda *= 0.5;
                    fails = 0;
                    if (lambda < 0.0001) lambda = 0.0001;
                } else {
                    fails++;
                    lambda *= 4;
                    if (lambda > 10000) lambda = 10000;
                }

                bool quitOpt = false;

                if (!(inc.norm() > eps) || iteration >= maxIterations[lvl] || fails >= 2) {
                    Mat88f H, Hsc;
                    Vec8f b, bsc;

                    quitOpt = true;
                }


                if (quitOpt) break;
                iteration++;
            }
            latestRes = resOld;

        }

        thisToNext = refToNew_current;
        thisToNext_aff = refToNew_aff_current;

        for (int i = 0; i < pyrLevelsUsed - 1; i++)
            propagateUp(i);

        frameID++;
        if (!snapped) snappedAt = 0;

        if (snapped && snappedAt == 0)
            snappedAt = frameID;

        return snapped && frameID > snappedAt + 5;
    }

    // calculates residual, Hessian and Hessian-block neede for re-substituting depth.
    Vec3f CoarseInitializer::calcResAndGS(
            int lvl, Mat88f &H_out, Vec8f &b_out,
            Mat88f &H_out_sc, Vec8f &b_out_sc,
            const SE3 &refToNew, AffLight refToNew_aff,
            bool plot) {
        int wl = w[lvl], hl = h[lvl];
        Eigen::Vector3f *colorRef = firstFrame->dIp[lvl];
        Eigen::Vector3f *colorNew = newFrame->dIp[lvl];

        Mat33f RKi = (refToNew.rotationMatrix() * Ki[lvl]).cast<float>();
        Vec3f t = refToNew.translation().cast<float>();
        Eigen::Vector2f r2new_aff = Eigen::Vector2f(exp(refToNew_aff.a), refToNew_aff.b);

        float fxl = fx[lvl];
        float fyl = fy[lvl];
        float cxl = cx[lvl];
        float cyl = cy[lvl];

        Accumulator11 E;
        // acc9 包含 H b
        acc9.initialize();
        // E 包含一个 A(float) 
        E.initialize();


        int npts  = numPoints[lvl];
        Pnt *ptsl = points[lvl];
        for (int i = 0; i < npts; i++) {

            Pnt *point = ptsl + i;

            point->maxstep = 1e10;
            // 如果该特征点不是good
            if (!point->isGood) {
                // 这个地方加的0
                E.updateSingle((float) (point->energy[0]));
                point->energy_new = point->energy;
                point->isGood_new = false;
                continue;
            }

            VecNRf dp0;
            VecNRf dp1;
            VecNRf dp2;
            VecNRf dp3;
            VecNRf dp4;
            VecNRf dp5;
            VecNRf dp6;
            VecNRf dp7;
            VecNRf dd;
            VecNRf r;
            JbBuffer_new[i].setZero();

            // sum over all residuals.
            bool isGood = true;
            float energy = 0;
            for (int idx = 0; idx < patternNum; idx++) {
                int dx = patternP[idx][0];
                int dy = patternP[idx][1];

                // 这边公式用的是齐次的公式，也就是Pc=K*T*Pw, Pw=[x y 1 \rho]^T
                Vec3f pt = RKi * Vec3f(point->u + dx, point->v + dy, 1) + t * point->idepth_new;

                // 投影到当前帧
                float u = pt[0] / pt[2];
                float v = pt[1] / pt[2];
                float Ku = fxl * u + cxl;
                float Kv = fyl * v + cyl;

                // 中间变量
                float new_idepth = point->idepth_new / pt[2];

                if (!(Ku > 1 && Kv > 1 && Ku < wl - 2 && Kv < hl - 2 && new_idepth > 0)) {
                    isGood = false;
                    break;
                }

                // 得到插值后的值
                // hitColor是一个vector3f，[0]：gray value，[1]: gradientX，[2]: gradientY
                Vec3f hitColor = getInterpolatedElement33(colorNew, Ku, Kv, wl);
                //Vec3f hitColor = getInterpolatedElement33BiCub(colorNew, Ku, Kv, wl);

                //float rlR = colorRef[point->u+dx + (point->v+dy) * wl][0];
                // 得到插值之后的灰度值
                float rlR = getInterpolatedElement31(colorRef, point->u + dx, point->v + dy, wl);

                if (!std::isfinite(rlR) || !std::isfinite((float) hitColor[0])) {
                    isGood = false;
                    break;
                }

                // 得到光度误差，有huber函数
                float residual = hitColor[0] - r2new_aff[0] * rlR - r2new_aff[1];
                float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);
                energy += hw * residual * residual * (2 - hw);

                // 中间变量，为雅可比做准备，主要针对深度的jacobian
                float dxdd = (t[0] - t[2] * u) / pt[2];
                float dydd = (t[1] - t[2] * v) / pt[2];

                if (hw < 1) hw = sqrtf(hw);
                float dxInterp = hw * hitColor[1] * fxl;
                float dyInterp = hw * hitColor[2] * fyl;

                // 0-5是6FOD(位姿) 
                dp0[idx] = new_idepth * dxInterp;
                dp1[idx] = new_idepth * dyInterp;
                dp2[idx] = -new_idepth * (u * dxInterp + v * dyInterp);
                dp3[idx] = -u * v * dxInterp - (1 + v * v) * dyInterp;
                dp4[idx] = (1 + u * u) * dxInterp + u * v * dyInterp;
                dp5[idx] = -v * dxInterp + u * dyInterp;

                // 对光度系数a b进行求导
                dp6[idx] = -hw * r2new_aff[0] * rlR;
                dp7[idx] = -hw * 1;

                // 逆深度的求导
                dd[idx] = dxInterp * dxdd + dyInterp * dydd;

                // insentity error
                r[idx] = hw * residual;

                float maxstep = 1.0f / Vec2f(dxdd * fxl, dydd * fyl).norm();
                if (maxstep < point->maxstep) point->maxstep = maxstep;

                // immediately compute dp*dd' and dd*dd' in JbBuffer1.
                // H = J^T*J, shape(J)=1*10
                // 因为要把逆深度merge掉用于加速计算，因此这里算的其实是W阵
                // 这里要merge掉深度值
                JbBuffer_new[i][0] += dp0[idx] * dd[idx];
                JbBuffer_new[i][1] += dp1[idx] * dd[idx];
                JbBuffer_new[i][2] += dp2[idx] * dd[idx];
                JbBuffer_new[i][3] += dp3[idx] * dd[idx];
                JbBuffer_new[i][4] += dp4[idx] * dd[idx];
                JbBuffer_new[i][5] += dp5[idx] * dd[idx];
                JbBuffer_new[i][6] += dp6[idx] * dd[idx];
                JbBuffer_new[i][7] += dp7[idx] * dd[idx];

                // Jb part
                JbBuffer_new[i][8] += r[idx] * dd[idx];
                
                // Hv part
                JbBuffer_new[i][9] += dd[idx] * dd[idx];
            }

            // 如果经过pattern运算之后判断出该点不好
            // 或者光度差太大了，应该是怕这个点主导了整个迭代的方向
            if (!isGood || energy > point->outlierTH * 20) {
                E.updateSingle((float) (point->energy[0]));
                point->isGood_new = false;
                point->energy_new = point->energy;
                continue;
            }


            // add into energy.
            E.updateSingle(energy);
            point->isGood_new = true;
            point->energy_new[0] = energy;

            // update Hessian matrix.
            // 使用SSE一次处理4个数据
            // 构建Hu矩阵，就是不被merge的部分
            for (int i = 0; i + 3 < patternNum; i += 4)
                acc9.updateSSE(
                        _mm_load_ps(((float *) (&dp0)) + i),
                        _mm_load_ps(((float *) (&dp1)) + i),
                        _mm_load_ps(((float *) (&dp2)) + i),
                        _mm_load_ps(((float *) (&dp3)) + i),
                        _mm_load_ps(((float *) (&dp4)) + i),
                        _mm_load_ps(((float *) (&dp5)) + i),
                        _mm_load_ps(((float *) (&dp6)) + i),
                        _mm_load_ps(((float *) (&dp7)) + i),
                        _mm_load_ps(((float *) (&r)) + i));

            // 处理剩下的数据，因为pattern有时候是奇数
            for (int i = ((patternNum >> 2) << 2); i < patternNum; i++)
                acc9.updateSingle(
                        (float) dp0[i], (float) dp1[i], (float) dp2[i], (float) dp3[i],
                        (float) dp4[i], (float) dp5[i], (float) dp6[i], (float) dp7[i],
                        (float) r[i]);


        }

        // 构建完整的H矩阵
        E.finish();
        acc9.finish();

        // calculate alpha energy, and decide if we cap it.
        Accumulator11 EAlpha;
        EAlpha.initialize();
        for (int i = 0; i < npts; i++) {
            Pnt *point = ptsl + i;
            if (!point->isGood_new) {
                E.updateSingle((float) (point->energy[1]));
            } else {
                point->energy_new[1] = (point->idepth_new - 1) * (point->idepth_new - 1);
                E.updateSingle((float) (point->energy_new[1]));
            }
        }
        EAlpha.finish();
        float alphaEnergy = alphaW * (EAlpha.A + refToNew.translation().squaredNorm() * npts);

        // compute alpha opt.
        float alphaOpt;
        if (alphaEnergy > alphaK * npts) {
            alphaOpt = 0;
            alphaEnergy = alphaK * npts;
        } else {
            alphaOpt = alphaW;
        }

        acc9SC.initialize();
        for (int i = 0; i < npts; i++) {
            Pnt *point = ptsl + i;
            if (!point->isGood_new)
                continue;

            point->lastHessian_new = JbBuffer_new[i][9];

            JbBuffer_new[i][8] += alphaOpt * (point->idepth_new - 1);
            JbBuffer_new[i][9] += alphaOpt;

            if (alphaOpt == 0) {
                JbBuffer_new[i][8] += couplingWeight * (point->idepth_new - point->iR);
                JbBuffer_new[i][9] += couplingWeight;
            }

            JbBuffer_new[i][9] = 1 / (1 + JbBuffer_new[i][9]);
            acc9SC.updateSingleWeighted(
                    (float) JbBuffer_new[i][0], (float) JbBuffer_new[i][1], (float) JbBuffer_new[i][2],
                    (float) JbBuffer_new[i][3],
                    (float) JbBuffer_new[i][4], (float) JbBuffer_new[i][5], (float) JbBuffer_new[i][6],
                    (float) JbBuffer_new[i][7],
                    (float) JbBuffer_new[i][8], (float) JbBuffer_new[i][9]);
        }
        acc9SC.finish();

        H_out = acc9.H.topLeftCorner<8, 8>();// / acc9.num;
        b_out = acc9.H.topRightCorner<8, 1>();// / acc9.num;
        H_out_sc = acc9SC.H.topLeftCorner<8, 8>();// / acc9.num;
        b_out_sc = acc9SC.H.topRightCorner<8, 1>();// / acc9.num;

        H_out(0, 0) += alphaOpt * npts;
        H_out(1, 1) += alphaOpt * npts;
        H_out(2, 2) += alphaOpt * npts;

        Vec3f tlog = refToNew.log().head<3>().cast<float>();
        b_out[0] += tlog[0] * alphaOpt * npts;
        b_out[1] += tlog[1] * alphaOpt * npts;
        b_out[2] += tlog[2] * alphaOpt * npts;


        return Vec3f(E.A, alphaEnergy, E.num);
    }

    float CoarseInitializer::rescale() {
        float factor = 20 * thisToNext.translation().norm();
        return factor;
    }

    Vec3f CoarseInitializer::calcEC(int lvl) {
        if (!snapped) return Vec3f(0, 0, numPoints[lvl]);
        AccumulatorX<2> E;
        E.initialize();
        int npts = numPoints[lvl];
        for (int i = 0; i < npts; i++) {
            Pnt *point = points[lvl] + i;
            if (!point->isGood_new) continue;
            float rOld = (point->idepth - point->iR);
            float rNew = (point->idepth_new - point->iR);
            E.updateNoWeight(Vec2f(rOld * rOld, rNew * rNew));

        }
        E.finish();

        return Vec3f(couplingWeight * E.A1m[0], couplingWeight * E.A1m[1], E.num);
    }

    void CoarseInitializer::optReg(int lvl) {
        int npts = numPoints[lvl];
        Pnt *ptsl = points[lvl];
        if (!snapped) {
            for (int i = 0; i < npts; i++)
                ptsl[i].iR = 1;
            return;
        }

        for (int i = 0; i < npts; i++) {
            Pnt *point = ptsl + i;
            if (!point->isGood) continue;

            float idnn[10];
            int nnn = 0;
            for (int j = 0; j < 10; j++) {
                if (point->neighbours[j] == -1) continue;
                Pnt *other = ptsl + point->neighbours[j];
                if (!other->isGood) continue;
                idnn[nnn] = other->iR;
                nnn++;
            }

            if (nnn > 2) {
                std::nth_element(idnn, idnn + nnn / 2, idnn + nnn);
                point->iR = (1 - regWeight) * point->idepth + regWeight * idnn[nnn / 2];
            }
        }

    }


    void CoarseInitializer::propagateUp(int srcLvl) {
        assert(srcLvl + 1 < pyrLevelsUsed);
        // set idepth of target

        int nptss = numPoints[srcLvl];
        int nptst = numPoints[srcLvl + 1];
        Pnt *ptss = points[srcLvl];
        Pnt *ptst = points[srcLvl + 1];

        // set to zero.
        for (int i = 0; i < nptst; i++) {
            Pnt *parent = ptst + i;
            parent->iR = 0;
            parent->iRSumNum = 0;
        }

        for (int i = 0; i < nptss; i++) {
            Pnt *point = ptss + i;
            if (!point->isGood) continue;

            Pnt *parent = ptst + point->parent;
            parent->iR += point->iR * point->lastHessian;
            parent->iRSumNum += point->lastHessian;
        }

        for (int i = 0; i < nptst; i++) {
            Pnt *parent = ptst + i;
            if (parent->iRSumNum > 0) {
                parent->idepth = parent->iR = (parent->iR / parent->iRSumNum);
                parent->isGood = true;
            }
        }

        optReg(srcLvl + 1);
    }

    void CoarseInitializer::propagateDown(int srcLvl) {
        assert(srcLvl > 0);
        // set idepth of target

        // numPoints表示每层有多少特征点
        // ptss表示当前层的上一层
        // ptst表示当前层
        int nptst = numPoints[srcLvl - 1];
        Pnt *ptss = points[srcLvl];
        Pnt *ptst = points[srcLvl - 1];

        for (int i = 0; i < nptst; i++) {
            Pnt *point = ptst + i;
            Pnt *parent = ptss + point->parent;

            if (!parent->isGood || parent->lastHessian < 0.1) continue;
            if (!point->isGood) {
                point->iR = point->idepth = point->idepth_new = parent->iR;
                point->isGood = true;
                point->lastHessian = 0;
            } else {
                float newiR = (point->iR * point->lastHessian * 2 + parent->iR * parent->lastHessian) /
                              (point->lastHessian * 2 + parent->lastHessian);
                point->iR = point->idepth = point->idepth_new = newiR;
            }
        }
        optReg(srcLvl - 1);
    }


    void CoarseInitializer::makeGradients(Eigen::Vector3f **data) {
        for (int lvl = 1; lvl < pyrLevelsUsed; lvl++) {
            int lvlm1 = lvl - 1;
            int wl = w[lvl], hl = h[lvl], wlm1 = w[lvlm1];

            Eigen::Vector3f *dINew_l = data[lvl];
            Eigen::Vector3f *dINew_lm = data[lvlm1];

            for (int y = 0; y < hl; y++)
                for (int x = 0; x < wl; x++)
                    dINew_l[x + y * wl][0] = 0.25f * (dINew_lm[2 * x + 2 * y * wlm1][0] +
                                                      dINew_lm[2 * x + 1 + 2 * y * wlm1][0] +
                                                      dINew_lm[2 * x + 2 * y * wlm1 + wlm1][0] +
                                                      dINew_lm[2 * x + 1 + 2 * y * wlm1 + wlm1][0]);

            for (int idx = wl; idx < wl * (hl - 1); idx++) {
                dINew_l[idx][1] = 0.5f * (dINew_l[idx + 1][0] - dINew_l[idx - 1][0]);
                dINew_l[idx][2] = 0.5f * (dINew_l[idx + wl][0] - dINew_l[idx - wl][0]);
            }
        }
    }

    void CoarseInitializer::setFirst(shared_ptr<CalibHessian> HCalib, shared_ptr<FrameHessian> newFrameHessian) {
        // 生成金字塔下图像的K矩阵
        makeK(HCalib);
        firstFrame = newFrameHessian;

        PixelSelector sel(w[0], h[0]);

        float *statusMap = new float[w[0] * h[0]];
        bool *statusMapB = new bool[w[0] * h[0]];

        float densities[] = {0.03, 0.05, 0.15, 0.5, 1}; // 9216  15360 64080
        for (int lvl = 0; lvl < pyrLevelsUsed; lvl++) {
            sel.currentPotential = 3;
            int npts;
            if (lvl == 0) {
                /// 金字塔第0层，640*480的图像大概取9000+个点
                /// statusMap表示每个像素的状态，为0表示没有点，1 2 4表示金字塔的层数
                npts = sel.makeMaps(firstFrame, statusMap, densities[lvl] * w[0] * h[0], 1, false, 2); 
            } else {
                /// 其他层，
                npts = makePixelStatus(firstFrame->dIp[lvl], statusMapB, w[lvl], h[lvl], densities[lvl] * w[0] * h[0]);
            }

            if (points[lvl] != 0) delete[] points[lvl];
            // 存储所有点的信息
            points[lvl] = new Pnt[npts];

            // set idepth map to initially 1 everywhere.
            int  wl = w[lvl], hl = h[lvl];
            Pnt *pl = points[lvl];
            int  nl = 0;
            // 对每个点进行遍历
            for (int y = patternPadding + 1; y < hl - patternPadding - 2; y++) {
                for (int x = patternPadding + 1; x < wl - patternPadding - 2; x++) {
                    if ((lvl != 0 && statusMapB[x + y * wl]) || (lvl == 0 && statusMap[x + y * wl] != 0)) {
                        //assert(patternNum==9);
                        pl[nl].u = x + 0.1;
                        pl[nl].v = y + 0.1;
                        pl[nl].idepth = 1;
                        pl[nl].iR = 1;
                        pl[nl].isGood = true;    // 第一次找到的点认为是good的
                        pl[nl].energy.setZero(); // Vector2f
                        pl[nl].lastHessian = 0;
                        pl[nl].lastHessian_new = 0;
                        pl[nl].my_type = (lvl != 0) ? 1 : statusMap[x + y * wl];

                        Eigen::Vector3f *cpt = firstFrame->dIp[lvl] + x + y * w[lvl];
                        float sumGrad2 = 0;
                        // patternNum = 8, SSE pattern
                        // 不知道这块是在干啥
                        for (int idx = 0; idx < patternNum; idx++) {
                            int dx = patternP[idx][0];
                            int dy = patternP[idx][1];
                            float absgrad = cpt[dx + dy * w[lvl]].tail<2>().squaredNorm();
                            sumGrad2 += absgrad;
                        }

                        pl[nl].outlierTH = patternNum * setting_outlierTH;

                        nl++;
                        assert(nl <= npts);
                    }
                }
            }

            numPoints[lvl] = nl;
        }
        delete[] statusMap;
        delete[] statusMapB;

        // 应该是在，构成KD树
        makeNN();

        thisToNext = SE3();
        snapped = false;
        frameID = snappedAt = 0;

        for (int i = 0; i < pyrLevelsUsed; i++)
            dGrads[i].setZero();

    }

    void CoarseInitializer::resetPoints(int lvl) {
        Pnt *pts = points[lvl];
        int npts = numPoints[lvl];
        for (int i = 0; i < npts; i++) {
            pts[i].energy.setzero();
            pts[i].idepth_new = pts[i].idepth;

            // 如果特征点位于顶层且不是good的话，看一下他的邻居怎么样
            // 如果邻居点不错的话，可以用邻居点的深度来代替
            // 自然，邻居点不能离该特征点太远
            if (lvl == pyrLevelsUsed - 1 && !pts[i].isGood) {
                float snd = 0, sn = 0;
                for (int n = 0; n < 10; n++) {
                    // 看一下相邻点的good比例
                    if (pts[i].neighbours[n] == -1 || !pts[pts[i].neighbours[n]].isgood) continue;
                    snd += pts[pts[i].neighbours[n]].ir;
                    sn += 1;
                }
                // 有一个是good就行
                if (sn > 0) {
                    pts[i].isgood = true;
                    // 初始化该点的深度
                    pts[i].ir = pts[i].idepth = pts[i].idepth_new = snd / sn;
                }
            }
        }
    }

    void coarseinitializer::dostep(int lvl, float lambda, vec8f inc) {

        const float maxpixelstep = 0.25;
        const float idmaxstep = 1e10;
        pnt *pts = points[lvl];
        int npts = numpoints[lvl];
        for (int i = 0; i < npts; i++) {
            if (!pts[i].isgood) continue;


            float b = jbbuffer[i][8] + jbbuffer[i].head<8>().dot(inc);
            float step = -b * jbbuffer[i][9] / (1 + lambda);


            float maxstep = maxpixelstep * pts[i].maxstep;
            if (maxstep > idmaxstep) maxstep = idmaxstep;

            if (step > maxstep) step = maxstep;
            if (step < -maxstep) step = -maxstep;

            float newidepth = pts[i].idepth + step;
            if (newidepth < 1e-3) newidepth = 1e-3;
            if (newidepth > 50) newidepth = 50;
            pts[i].idepth_new = newidepth;
        }

    }

    void coarseinitializer::applystep(int lvl) {
        pnt *pts = points[lvl];
        int npts = numpoints[lvl];
        for (int i = 0; i < npts; i++) {
            if (!pts[i].isgood) {
                pts[i].idepth = pts[i].idepth_new = pts[i].ir;
                continue;
            }
            pts[i].energy = pts[i].energy_new;
            pts[i].isgood = pts[i].isgood_new;
            pts[i].idepth = pts[i].idepth_new;
            pts[i].lasthessian = pts[i].lasthessian_new;
        }
        std::swap<vec10f *>(jbbuffer, jbbuffer_new);
    }

    void coarseinitializer::makek(shared_ptr<calibhessian> hcalib) {
        w[0] = wg[0];
        h[0] = hg[0];

        fx[0] = hcalib->fxl();
        fy[0] = hcalib->fyl();
        cx[0] = hcalib->cxl();
        cy[0] = hcalib->cyl();

        for (int level = 1; level < pyrlevelsused; ++level) {
            w[level] = w[0] >> level;
            h[level] = h[0] >> level;
            fx[level] = fx[level - 1] * 0.5;
            fy[level] = fy[level - 1] * 0.5;
            cx[level] = (cx[0] + 0.5) / ((int) 1 << level) - 0.5;
            cy[level] = (cy[0] + 0.5) / ((int) 1 << level) - 0.5;
        }

        for (int level = 0; level < pyrlevelsused; ++level) {
            k[level] << fx[level], 0.0, cx[level], 0.0, fy[level], cy[level], 0.0, 0.0, 1.0;
            ki[level] = k[level].inverse();
            fxi[level] = ki[level](0, 0);
            fyi[level] = ki[level](1, 1);
            cxi[level] = ki[level](0, 2);
            cyi[level] = ki[level](1, 2);
        }
    }

    void coarseinitializer::makenn() {
        const float nndistfactor = 0.05;

        typedef nanoflann::kdtreesingleindexadaptor<
                nanoflann::l2_simple_adaptor<float, flannpointcloud>,
                flannpointcloud, 2> kdtree;

        // build indices
        flannpointcloud pcs[pyr_levels];
        kdtree *indexes[pyr_levels];
        for (int i = 0; i < pyrlevelsused; i++) {
            pcs[i] = flannpointcloud(numpoints[i], points[i]);
            indexes[i] = new kdtree(2, pcs[i], nanoflann::kdtreesingleindexadaptorparams(5));
            indexes[i]->buildindex();
        }

        // 找10个邻居
        const int nn = 10;

        // find nn & parents
        for (int lvl = 0; lvl < pyrlevelsused; lvl++) {
            pnt *pts = points[lvl];
            int npts = numpoints[lvl];

            int ret_index[nn];
            float ret_dist[nn];
            nanoflann::knnresultset<float, int, int> resultset(nn);
            nanoflann::knnresultset<float, int, int> resultset1(1);

            for (int i = 0; i < npts; i++) {
                //resultset.init(pts[i].neighbours, pts[i].neighboursdist );
                // 在同层找前十个邻居点
                resultset.init(ret_index, ret_dist);
                vec2f pt = vec2f(pts[i].u, pts[i].v);
                indexes[lvl]->findneighbors(resultset, (float *) &pt, nanoflann::searchparams());
                int myidx = 0;
                float sumDF = 0;
                // 设置nn个邻居，
                for (int k = 0; k < nn; k++) {
                    pts[i].neighbours[myidx] = ret_index[k];
                    float df = expf(-ret_dist[k] * nndistfactor);
                    sumdf += df;
                    pts[i].neighboursdist[myidx] = df;
                    assert(ret_index[k] >= 0 && ret_index[k] < npts);
                    myidx++;
                }
                for (int k = 0; k < nn; k++)
                    pts[i].neighboursdist[k] *= 10 / sumdf;

                if (lvl < pyrLevelsUsed - 1) {
                    resultSet1.init(ret_index, ret_dist);
                    pt = pt * 0.5f - Vec2f(0.25f, 0.25f);
                    indexes[lvl + 1]->findNeighbors(resultSet1, (float *) &pt, nanoflann::SearchParams());

                    pts[i].parent = ret_index[0];
                    pts[i].parentdist = expf(-ret_dist[0] * nndistfactor);

                    assert(ret_index[0] >= 0 && ret_index[0] < numpoints[lvl + 1]);
                } else {
                    pts[i].parent = -1;
                    pts[i].parentdist = -1;
                }
            }
        }
        // done.

        for (int i = 0; i < pyrlevelsused; i++)
            delete indexes[i];
    }
