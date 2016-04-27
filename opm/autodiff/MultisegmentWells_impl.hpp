/*
  Copyright 2016 SINTEF ICT, Applied Mathematics.
  Copyright 2016 Statoil ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_MULTISEGMENTWELLS_IMPL_HEADER_INCLUDED
#define OPM_MULTISEGMENTWELLS_IMPL_HEADER_INCLUDED


namespace Opm
{



    namespace wellhelpers {

        using ADB = MultisegmentWells::ADB;
        using Vector = MultisegmentWells::Vector;

        inline
        ADB onlyWellDerivs(const ADB& x)
        {
            Vector val = x.value();
            const int nb = x.numBlocks();
            if (nb < 2) {
                OPM_THROW(std::logic_error, "Called onlyWellDerivs() with argument that has " << nb << " blocks.");
            }
            std::vector<ADB::M> derivs = { x.derivative()[nb - 2], x.derivative()[nb - 1] };
            return ADB::function(std::move(val), std::move(derivs));
        }
    }



    template <class WellState>
    void
    MultisegmentWells::
    updateWellState(const Vector& dwells,
                    const int np,
                    const double dpmaxrel,
                    WellState& well_state) const
    {
        if (!wells().empty())
        {
            const int nw = wells().size();
            const int nseg_total = nseg_total_;

            // Extract parts of dwells corresponding to each part.
            int varstart = 0;
            const Vector dsegqs = subset(dwells, Span(np * nseg_total, 1, varstart));
            varstart += dsegqs.size();
            const Vector dsegp = subset(dwells, Span(nseg_total, 1, varstart));
            varstart += dsegp.size();
            assert(varstart == dwells.size());


            // segment phase rates update
            // in dwells, the phase rates are ordered by phase.
            // while in WellStateMultiSegment, the phase rates are ordered by segments
            const DataBlock wsr = Eigen::Map<const DataBlock>(dsegqs.data(), np, nseg_total).transpose();
            const Vector dwsr = Eigen::Map<const Vector>(wsr.data(), nseg_total * np);
            const Vector wsr_old = Eigen::Map<const Vector>(&well_state.segPhaseRates()[0], nseg_total * np);
            const Vector sr = wsr_old - dwsr;
            std::copy(&sr[0], &sr[0] + sr.size(), well_state.segPhaseRates().begin());


            // segment pressure updates
            const Vector segp_old = Eigen::Map<const Vector>(&well_state.segPress()[0], nseg_total, 1);
            // TODO: applying the pressure change limiter to all the segments, not sure if it is the correct thing to do
            const Vector dsegp_limited = sign(dsegp) * dsegp.abs().min(segp_old.abs() * dpmaxrel);
            const Vector segp = segp_old - dsegp_limited;
            std::copy(&segp[0], &segp[0] + segp.size(), well_state.segPress().begin());

            // update the well rates and bhps, which are not anymore primary vabriables.
            // they are updated directly from the updated segment phase rates and segment pressures.

            // Bhp update.
            Vector bhp = Vector::Zero(nw);
            Vector wr = Vector::Zero(nw * np);
            // it is better to use subset

            int start_segment = 0;
            for (int w = 0; w < nw; ++w) {
                bhp[w] = well_state.segPress()[start_segment];
                // insert can be faster
                for (int p = 0; p < np; ++p) {
                    wr[p + np * w] = well_state.segPhaseRates()[p + np * start_segment];
                }

                const int nseg = wells()[w]->numberOfSegments();
                start_segment += nseg;
            }

            assert(start_segment == nseg_total);
            std::copy(&bhp[0], &bhp[0] + bhp.size(), well_state.bhp().begin());
            std::copy(&wr[0], &wr[0] + wr.size(), well_state.wellRates().begin());

            // TODO: handling the THP control related.
        }
    }





    template <class SolutionState>
    void
    MultisegmentWells::
    computeWellFlux(const SolutionState& state,
                    const Opm::PhaseUsage& pu,
                    const std::vector<bool>& active,
                    const Vector& well_perforation_pressure_diffs,
                    const DataBlock& compi,
                    const std::vector<ADB>& mob_perfcells,
                    const std::vector<ADB>& b_perfcells,
                    const int np,
                    Vector& aliveWells,
                    std::vector<ADB>& cq_s) const
    {
        if (wells().size() == 0) return;

        const int nw = wells().size();

        aliveWells = Vector::Constant(nw, 1.0);

        const int nseg = nseg_total_;
        const int nperf = nperf_total_;

        cq_s.resize(np, ADB::null());

        {
            const Vector& Tw = wellOps().conn_trans_factors;
            const std::vector<int>& well_cells = wellOps().well_cells;

            // determining in-flow (towards well-bore) or out-flow (towards reservoir)
            // for mutli-segmented wells and non-segmented wells, the calculation of the drawdown are different.
            const ADB& p_perfcells = subset(state.pressure, well_cells);
            const ADB& rs_perfcells = subset(state.rs, well_cells);
            const ADB& rv_perfcells = subset(state.rv, well_cells);

            const ADB& seg_pressures = state.segp;

            const ADB seg_pressures_perf = wellOps().s2p * seg_pressures;

            // Create selector for perforations of multi-segment vs. regular wells.
            Vector is_multisegment_well(nw);
            for (int w = 0; w < nw; ++w) {
                is_multisegment_well[w] = double(wells()[w]->isMultiSegmented());
            }
            // Take one flag per well and expand to one flag per perforation.
            Vector is_multisegment_perf = wellOps().w2p * is_multisegment_well.matrix();
            Selector<double> msperf_selector(is_multisegment_perf, Selector<double>::NotEqualZero);

            // Compute drawdown.
            ADB h_nc = msperf_selector.select(wellSegmentPerforationPressureDiffs(),
                                              ADB::constant(well_perforation_pressure_diffs));
            const Vector h_cj = msperf_selector.select(wellPerforationCellPressureDiffs(), Vector::Zero(nperf));

            // Special handling for when we are called from solveWellEq().
            // TODO: restructure to eliminate need for special treatmemt.
            if ((h_nc.numBlocks() != 0) && (h_nc.numBlocks() != seg_pressures_perf.numBlocks())) {
                assert(seg_pressures_perf.numBlocks() == 2);
                assert(h_nc.numBlocks() > 2);
                h_nc = wellhelpers::onlyWellDerivs(h_nc);
                assert(h_nc.numBlocks() == 2);
            }

            ADB drawdown = (p_perfcells + h_cj - seg_pressures_perf - h_nc);

            // selects injection perforations
            Vector selectInjectingPerforations = Vector::Zero(nperf);
            // selects producing perforations
            Vector selectProducingPerforations = Vector::Zero(nperf);
            for (int c = 0; c < nperf; ++c){
                if (drawdown.value()[c] < 0)
                    selectInjectingPerforations[c] = 1;
                else
                    selectProducingPerforations[c] = 1;
            }

            // handling flow into wellbore
            // maybe there are something to do there make the procedure easier.
            std::vector<ADB> cq_ps(np, ADB::null());
            for (int phase = 0; phase < np; ++phase) {
                const ADB cq_p = -(selectProducingPerforations * Tw) * (mob_perfcells[phase] * drawdown);
                cq_ps[phase] = b_perfcells[phase] * cq_p;
            }

            if (active[Oil] && active[Gas]) {
                const int oilpos = pu.phase_pos[Oil];
                const int gaspos = pu.phase_pos[Gas];
                const ADB cq_psOil = cq_ps[oilpos];
                const ADB cq_psGas = cq_ps[gaspos];
                cq_ps[gaspos] += rs_perfcells * cq_psOil;
                cq_ps[oilpos] += rv_perfcells * cq_psGas;
            }

            // hadling flow out from wellbore
            ADB total_mob = mob_perfcells[0];
            for (int phase = 1; phase < np; ++phase) {
                total_mob += mob_perfcells[phase];
            }

            // injection perforations total volume rates
            const ADB cqt_i = -(selectInjectingPerforations * Tw) * (total_mob * drawdown);

            // compute wellbore mixture for injecting perforations
            // The wellbore mixture depends on the inflow from the reservoir
            // and the well injection rates.
            // TODO: should this based on the segments?
            // TODO: for the usual wells, the well rates are the sum of the perforations.
            // TODO: for multi-segmented wells, the segment rates are not the sum of the perforations.

            // TODO: two options here
            // TODO: 1. for each segment, only the inflow from the perforations related to this segment are considered.
            // TODO: 2. for each segment, the inflow from the perforrations related to this segment and also all the inflow
            // TODO: from the upstreaming sgments and their perforations need to be considered.
            // TODO: This way can be the more consistent way, while let us begin with the first option. The second option
            // TODO: involves one operations that are not valid now. (i.e. how to transverse from the leaves to the root,
            // TODO: although we can begin from the brutal force way)

            // TODO: stop using wells() here.
            std::vector<ADB> wbq(np, ADB::null());
            ADB wbqt = ADB::constant(Vector::Zero(nseg));

            for (int phase = 0; phase < np; ++phase) {
                const ADB& q_ps = wellOps().p2s * cq_ps[phase];
                const ADB& q_s = subset(state.segqs, Span(nseg, 1, phase * nseg));
                Selector<double> injectingPhase_selector(q_s.value(), Selector<double>::GreaterZero);

                const int pos = pu.phase_pos[phase];

                // this is per segment
                wbq[phase] = (wellOps().w2s * ADB::constant(compi.col(pos)) * injectingPhase_selector.select(q_s, ADB::constant(Vector::Zero(nseg)))) - q_ps;

                // TODO: it should be a single value for this certain well.
                // TODO: it need to be changed later to handle things more consistently
                // or there should be an earsier way to decide if the well is dead.
                wbqt += wbq[phase];
            }

            // Set aliveWells.
            // the first value of the wbqt is the one to decide if the well is dead
            // or there should be some dead segments?
            {
                int topseg = 0;
                for (int w = 0; w < nw; ++w) {
                    if (wbqt.value()[topseg] == 0.0) { // yes we really mean == here, no fuzzyness
                        aliveWells[w] = 0.0;
                    }
                    topseg += wells()[w]->numberOfSegments();
                }
            }

            // compute wellbore mixture at standard conditions.
            // before, the determination of alive wells is based on wells.
            // now, will there be any dead segment? I think no.
            // TODO: it is not clear if the cmix_s should be based on segment or the well
            std::vector<ADB> cmix_s(np, ADB::null());
            Selector<double> aliveWells_selector(aliveWells, Selector<double>::NotEqualZero);
            for (int phase = 0; phase < np; ++phase) {
                const int pos = pu.phase_pos[phase];
                const ADB phase_fraction = wellOps().topseg2w * (wbq[phase] / wbqt);
                cmix_s[phase] = wellOps().w2p * aliveWells_selector.select(phase_fraction, ADB::constant(compi.col(pos)));
            }

            // compute volume ration between connection at standard conditions
            ADB volumeRatio = ADB::constant(Vector::Zero(nperf));
            const ADB d = Vector::Constant(nperf,1.0) -  rv_perfcells * rs_perfcells;

            for (int phase = 0; phase < np; ++phase) {
                ADB tmp = cmix_s[phase];
                if (phase == Oil && active[Gas]) {
                    const int gaspos = pu.phase_pos[Gas];
                    tmp = tmp - rv_perfcells * cmix_s[gaspos] / d;
                }
                if (phase == Gas && active[Oil]) {
                    const int oilpos = pu.phase_pos[Oil];
                    tmp = tmp - rs_perfcells * cmix_s[oilpos] / d;
                }
                volumeRatio += tmp / b_perfcells[phase];
            }

            // injecting connections total volumerates at standard conditions
            ADB cqt_is = cqt_i/volumeRatio;

            // connection phase volumerates at standard conditions
            for (int phase = 0; phase < np; ++phase) {
                cq_s[phase] = cq_ps[phase] + cmix_s[phase]*cqt_is;
            }
        }
    }





    template <class SolutionState>
    void
    MultisegmentWells::
    computeSegmentFluidProperties(const SolutionState& state,
                                  const std::vector<PhasePresence>& pc,
                                  const std::vector<bool>& active,
                                  const BlackoilPropsAdInterface& fluid,
                                  const int np)
    {
        const int nw = wells().size();
        const int nseg_total = nseg_total_;

        if ( !wellOps().has_multisegment_wells ){
            // not sure if this is needed actually
            // TODO: to check later if this is really necessary.
            wellSegmentDensities() = ADB::constant(Vector::Zero(nseg_total));
            segmentMassFlowRates() = ADB::constant(Vector::Zero(nseg_total));
            segmentViscosities() = ADB::constant(Vector::Zero(nseg_total));
            for (int phase = 0; phase < np; ++phase) {
                segmentCompSurfVolumeCurrent()[phase] = ADB::constant(Vector::Zero(nseg_total));
                segmentCompSurfVolumeInitial()[phase] = Vector::Zero(nseg_total);
            }
            return;
        }

        // although we will calculate segment density for non-segmented wells at the same time,
        // while under most of the cases, they will not be used,
        // since for most of the cases, the density calculation for non-segment wells are
        // set to be 'SEG' way, which is not a option for multi-segment wells.
        // When the density calcuation for non-segmented wells are set to 'AVG', then
        // the density calculation of the mixtures can be the same, while it remains to be verified.

        // The grid cells associated with segments.
        // TODO: shoud be computed once and stored in WellState or global Wells structure or class.
        std::vector<int> segment_cells;
        segment_cells.reserve(nseg_total);
        for (int w = 0; w < nw; ++w) {
            const std::vector<int>& segment_cells_well = wells()[w]->segmentCells();
            segment_cells.insert(segment_cells.end(), segment_cells_well.begin(), segment_cells_well.end());
        }
        assert(int(segment_cells.size()) == nseg_total);

        const ADB segment_temp = subset(state.temperature, segment_cells);
        // using the segment pressure or the average pressure
        // using the segment pressure first
        const ADB& segment_press = state.segp;

        // Compute PVT properties for segments.
        std::vector<PhasePresence> segment_cond(nseg_total);
        for (int s = 0; s < nseg_total; ++s) {
            segment_cond[s] = pc[segment_cells[s]];
        }
        std::vector<ADB> b_seg(np, ADB::null());
        // Viscosities for different phases
        std::vector<ADB> mu_seg(np, ADB::null());
        ADB rsmax_seg = ADB::null();
        ADB rvmax_seg = ADB::null();
        const PhaseUsage& pu = fluid.phaseUsage();
        if (pu.phase_used[Water]) {
            b_seg[pu.phase_pos[Water]] = fluid.bWat(segment_press, segment_temp, segment_cells);
            mu_seg[pu.phase_pos[Water]] = fluid.muWat(segment_press, segment_temp, segment_cells);
        }
        assert(active[Oil]);
        const ADB segment_so = subset(state.saturation[pu.phase_pos[Oil]], segment_cells);
        if (pu.phase_used[Oil]) {
            const ADB segment_rs = subset(state.rs, segment_cells);
            b_seg[pu.phase_pos[Oil]] = fluid.bOil(segment_press, segment_temp, segment_rs,
                                                   segment_cond, segment_cells);
            // rsmax_seg = fluidRsSat(segment_press, segment_so, segment_cells);
            rsmax_seg = fluid.rsSat(segment_press, segment_so, segment_cells);
            mu_seg[pu.phase_pos[Oil]] = fluid.muOil(segment_press, segment_temp, segment_rs,
                                                     segment_cond, segment_cells);
        }
        assert(active[Gas]);
        if (pu.phase_used[Gas]) {
            const ADB segment_rv = subset(state.rv, segment_cells);
            b_seg[pu.phase_pos[Gas]] = fluid.bGas(segment_press, segment_temp, segment_rv,
                                                   segment_cond, segment_cells);
            // rvmax_seg = fluidRvSat(segment_press, segment_so, segment_cells);
            rvmax_seg = fluid.rvSat(segment_press, segment_so, segment_cells);
            mu_seg[pu.phase_pos[Gas]] = fluid.muGas(segment_press, segment_temp, segment_rv,
                                                   segment_cond, segment_cells);
        }

        // Extract segment flow by phase (segqs) and compute total surface rate.
        ADB tot_surface_rate = ADB::constant(Vector::Zero(nseg_total));
        std::vector<ADB> segqs(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            segqs[phase] = subset(state.segqs, Span(nseg_total, 1, phase * nseg_total));
            tot_surface_rate += segqs[phase];
        }

        // TODO: later this will be implmented as a global mapping
        std::vector<std::vector<double>> comp_frac(np, std::vector<double>(nseg_total, 0.0));
        int start_segment = 0;
        for (int w = 0; w < nw; ++w) {
            WellMultiSegmentConstPtr well = wells()[w];
            const int nseg = well->numberOfSegments();
            const std::vector<double>& comp_frac_well = well->compFrac();
            for (int phase = 0; phase < np; ++phase) {
                for (int s = 0; s < nseg; ++s) {
                    comp_frac[phase][s + start_segment] = comp_frac_well[phase];
                }
            }
            start_segment += nseg;
        }
        assert(start_segment == nseg_total);

        // Compute mix.
        // 'mix' contains the component fractions under surface conditions.
        std::vector<ADB> mix(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            // initialize to be the compFrac for each well,
            // then update only the one with non-zero total volume rate
            mix[phase] = ADB::constant(Eigen::Map<Vector>(comp_frac[phase].data(), nseg_total));
        }
        // There should be a better way to do this.
        Selector<double> non_zero_tot_rate(tot_surface_rate.value(), Selector<double>::NotEqualZero);
        for (int phase = 0; phase < np; ++phase) {
            mix[phase] = non_zero_tot_rate.select(segqs[phase] / tot_surface_rate, mix[phase]);
        }

        // Calculate rs and rv.
        ADB rs = ADB::constant(Vector::Zero(nseg_total));
        ADB rv = rs;
        const int gaspos = pu.phase_pos[Gas];
        const int oilpos = pu.phase_pos[Oil];
        Selector<double> non_zero_mix_oilpos(mix[oilpos].value(), Selector<double>::GreaterZero);
        Selector<double> non_zero_mix_gaspos(mix[gaspos].value(), Selector<double>::GreaterZero);
        // What is the better way to do this?
        // big values should not be necessary
        ADB big_values = ADB::constant(Vector::Constant(nseg_total, 1.e100));
        ADB mix_gas_oil = non_zero_mix_oilpos.select(mix[gaspos] / mix[oilpos], big_values);
        ADB mix_oil_gas = non_zero_mix_gaspos.select(mix[oilpos] / mix[gaspos], big_values);
        if (active[Oil]) {
            Vector selectorUnderRsmax = Vector::Zero(nseg_total);
            Vector selectorAboveRsmax = Vector::Zero(nseg_total);
            for (int s = 0; s < nseg_total; ++s) {
                if (mix_gas_oil.value()[s] > rsmax_seg.value()[s]) {
                    selectorAboveRsmax[s] = 1.0;
                } else {
                    selectorUnderRsmax[s] = 1.0;
                }
            }
            rs = non_zero_mix_oilpos.select(selectorAboveRsmax * rsmax_seg + selectorUnderRsmax * mix_gas_oil, rs);
        }
        if (active[Gas]) {
            Vector selectorUnderRvmax = Vector::Zero(nseg_total);
            Vector selectorAboveRvmax = Vector::Zero(nseg_total);
            for (int s = 0; s < nseg_total; ++s) {
                if (mix_oil_gas.value()[s] > rvmax_seg.value()[s]) {
                    selectorAboveRvmax[s] = 1.0;
                } else {
                    selectorUnderRvmax[s] = 1.0;
                }
            }
            rv = non_zero_mix_gaspos.select(selectorAboveRvmax * rvmax_seg + selectorUnderRvmax * mix_oil_gas, rv);
        }

        // Calculate the phase fraction under reservoir conditions.
        std::vector<ADB> x(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            x[phase] = mix[phase];
        }
        if (active[Gas] && active[Oil]) {
            x[gaspos] = (mix[gaspos] - mix[oilpos] * rs) / (Vector::Ones(nseg_total) - rs * rv);
            x[oilpos] = (mix[oilpos] - mix[gaspos] * rv) / (Vector::Ones(nseg_total) - rs * rv);
        }

        // Compute total reservoir volume to surface volume ratio.
        ADB volrat = ADB::constant(Vector::Zero(nseg_total));
        for (int phase = 0; phase < np; ++phase) {
            volrat += x[phase] / b_seg[phase];
        }

        // Compute segment densities.
        ADB dens = ADB::constant(Vector::Zero(nseg_total));
        for (int phase = 0; phase < np; ++phase) {
            const Vector surface_density = fluid.surfaceDensity(phase, segment_cells);
            dens += surface_density * mix[phase];
        }
        wellSegmentDensities() = dens / volrat;

        // Calculating the surface volume of each component in the segment
        assert(np == int(segmentCompSurfVolumeCurrent().size()));
        const ADB segment_surface_volume = segVDt() / volrat;
        for (int phase = 0; phase < np; ++phase) {
            segmentCompSurfVolumeCurrent()[phase] = segment_surface_volume * mix[phase];
        }

        // Mass flow rate of the segments
        segmentMassFlowRates() = ADB::constant(Vector::Zero(nseg_total));
        for (int phase = 0; phase < np; ++phase) {
            // TODO: how to remove one repeated surfaceDensity()
            const Vector surface_density = fluid.surfaceDensity(phase, segment_cells);
            segmentMassFlowRates() += surface_density * segqs[phase];
        }

        // Viscosity of the fluid mixture in the segments
        segmentViscosities() = ADB::constant(Vector::Zero(nseg_total));
        for (int phase = 0; phase < np; ++phase) {
            segmentViscosities() += x[phase] * mu_seg[phase];
        }
    }

}
#endif // OPM_MULTISEGMENTWELLS_IMPL_HEADER_INCLUDED
