/*
 * Fusion X-Port — tools/dump_mfa.c
 * Diagnostic: prints the full structure of an MFA file.
 * Host-only build; links the same sources as the runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fxp/fxp.h"
#include "fxp/mfa.h"
#include "fxp/mfa_format.h"

static const char *type_name(int32_t t)
{
    switch (t) {
    case -7: return "player";   case -6: return "keyboard";
    case -5: return "create";   case -4: return "timer";
    case -3: return "game";     case -2: return "speaker";
    case -1: return "system";   case 0:  return "quick_backdrop";
    case 1:  return "backdrop"; case 2:  return "active";
    case 3:  return "text";     case 4:  return "question";
    case 5:  return "score";    case 6:  return "lives";
    case 7:  return "counter";  case 8:  return "rtf";
    case 9:  return "subapp";   default: return "extension";
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s file.mfa\n", argv[0]); return 1; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { perror("read"); return 1; }
    fclose(f);

    fxp_mfa_t *mfa = fxp_mfa_load(buf, (size_t)size);
    if (!mfa) { fprintf(stderr, "LOAD FAILED\n"); return 1; }

    printf("== MFA '%s' v%d.%d product %d build %d ==\n",
           mfa->name, mfa->version, mfa->subversion, mfa->product, mfa->build);
    printf("fonts=%d sounds=%d music=%d images=%d icons=%d\n",
           mfa->fonts.count, mfa->sounds.count, mfa->music_count,
           mfa->images.count, mfa->icon_count);

    printf("-- image bank (%d) --\n", mfa->images.count);
    for (int32_t i = 0; i < mfa->images.count; i++) {
        fxp_image_t *im = &mfa->images.items[i];
        printf("  [%d] handle=%d %dx%d mode=%u flags=%02x rgba=%s\n",
               i, im->handle, im->width, im->height, im->graphic_mode,
               im->flags, im->rgba ? "ok" : "NULL");
    }

    printf("-- sound bank (%d) --\n", mfa->sounds.count);
    for (int32_t i = 0; i < mfa->sounds.count; i++) {
        fxp_sound_t *s = &mfa->sounds.items[i];
        printf("  [%d] handle=%d flags=%02x data=%d bytes head:",
               i, s->handle, s->flags, s->data_size);
        for (int b = 0; b < 8 && b < s->data_size; b++)
            printf(" %02x", s->data[b]);
        printf("\n");
    }

    printf("global values (%d):", mfa->global_values.count);
    for (int32_t i = 0; i < mfa->global_values.count; i++)
        printf(" [%d]=%s", i,
               mfa->global_values.items[i].type == FXP_VTYPE_STRING
                   ? mfa->global_values.items[i].s
                   : "(num)");
    printf("\n");

    printf("-- player controls (%d) --\n", mfa->control_count);
    {
        static const char *cn[8] = { "Up", "Down", "Left", "Right",
                                     "Btn1", "Btn2", "Btn3", "Btn4" };
        for (int32_t p = 0; p < mfa->control_count && p < 4; p++) {
            printf("  player %d:", p + 1);
            for (int32_t c = 0; c < 16; c++) {
                int32_t vk = mfa->controls[p].keys[c];
                if (c < 8)
                    printf(" %s=%d(0x%02x)", cn[c], vk, vk);
                else if (vk)
                    printf(" k%d=%d", c, vk);
            }
            printf("\n");
        }
    }

    for (int32_t fi = 0; fi < mfa->frame_count; fi++) {
        fxp_mfa_frame_t *fr = mfa->frames[fi];
        printf("== frame %d '%s' %dx%d bg=%02x%02x%02x layers=%d items=%d instances=%d ==\n",
               fr->handle, fr->name, fr->size_x, fr->size_y,
               fr->background.r, fr->background.g, fr->background.b, fr->layer_count,
               fr->item_count, fr->instance_count);

        for (int32_t i = 0; i < fr->item_count; i++) {
            fxp_item_t *it = fr->items[i];
            printf("  item[%d] type=%s(%d) handle=%d name='%s'\n",
                   i, type_name(it->object_type), it->object_type,
                   it->handle, it->name);
            printf("    flags=%08x new_flags=%08x (visible_bit=%d) ink=%d\n",
                   it->flags, it->common.new_object_flags,
                   (it->common.new_object_flags >> 3) & 1, it->ink_effect);
            if (it->object_type == FXP_OT_ACTIVE) {
                printf("    anims=%d has=%u\n",
                       it->as.active.anim_count, it->as.active.has_anims);
                for (int32_t a = 0; a < it->as.active.anim_count; a++) {
                    fxp_anim_t *an = &it->as.active.anims[a];
                    printf("    anim[%d] '%s' dirs=%d\n", a, an->name, an->dir_count);
                    for (int32_t d = 0; d < an->dir_count; d++) {
                        fxp_anim_dir_t *dir = &an->dirs[d];
                        printf("      dir[%d] idx=%d speeds=[%d..%d] rep=%d back=%d frames=%d:",
                               d, dir->index, dir->min_speed, dir->max_speed,
                               dir->repeat, dir->back_to, dir->frame_count);
                        for (int32_t k = 0; k < dir->frame_count && k < 32; k++)
                            printf(" %d", dir->frames[k]);
                        printf("\n");
                    }
                }
            } else if (it->object_type == FXP_OT_BACKDROP) {
                printf("    image=%d obstacle=%d\n",
                       it->as.backdrop.image, it->as.backdrop.obstacle_type);
            } else if (it->object_type == FXP_OT_QUICK_BACKDROP) {
                printf("    %dx%d fill=%d\n",
                       it->as.quick_backdrop.w, it->as.quick_backdrop.h,
                       it->as.quick_backdrop.fill_type);
            } else if (it->object_type == FXP_OT_TEXT) {
                printf("    %dx%d font=%d paras=%d:\n",
                       it->as.text.w, it->as.text.h, it->as.text.font,
                       it->as.text.paragraph_count);
                for (int32_t p = 0; p < it->as.text.paragraph_count; p++)
                    printf("      '%s'\n", it->as.text.paragraphs[p].text);
            }
            printf("    values(%d):", it->common.values.count);
            for (int32_t v = 0; v < it->common.values.count; v++) {
                fxp_value_item_t *vi = &it->common.values.items[v];
                if (vi->type == FXP_VTYPE_STRING)
                    printf(" '%s'='%s'", vi->name, vi->s);
                else
                    printf(" '%s'=%g", vi->name,
                           vi->type == FXP_VTYPE_FLOAT ? vi->f : (double)vi->i);
            }
            printf("\n");
            printf("    movements(%d):", it->common.movement_count);
            for (int32_t m = 0; m < it->common.movement_count; m++)
                printf(" '%s' type=%u moving=%u dir=%d",
                       it->common.movements[m].name,
                       it->common.movements[m].type,
                       it->common.movements[m].moving_at_start,
                       it->common.movements[m].direction_at_start);
            printf("\n");
        }

        printf("  -- instances --\n");
        for (int32_t i = 0; i < fr->instance_count; i++) {
            fxp_mfa_instance_t *in = &fr->instances[i];
            fxp_item_t *it = fxp_mfa_frame_item(fr, (int32_t)in->item_handle);
            printf("   [%d] item_handle=%u (%s '%s') xy=(%d,%d) layer=%u handle=%d flags=%04x\n",
                   i, in->item_handle,
                   it ? type_name(it->object_type) : "???",
                   it ? it->name : "UNKNOWN",
                   in->x, in->y, in->layer, in->handle, in->flags);
        }

        if (fr->events.object_count > 0) {
            printf("  -- event object table (EvOb, %d rows) --\n",
                   fr->events.object_count);
            for (int32_t o = 0; o < fr->events.object_count; o++) {
                fxp_event_object_t *eo = &fr->events.objects[o];
                printf("   row handle=%u objType=%d itemHandle=%u qual=%u\n",
                       eo->handle, eo->object_type, eo->item_handle,
                       eo->system_qualifier);
            }
        }
        printf("  -- events: ver=%u type=%u groups=%d --\n",
               fr->events.version, fr->events.frame_type, fr->events.group_count);
        for (int32_t g = 0; g < fr->events.group_count; g++) {
            fxp_event_group_t *gr = &fr->events.groups[g];
            printf("   group[%d] cond=%u act=%u flags=%04x id=%d\n",
                   g, gr->condition_count, gr->action_count, gr->flags,
                   gr->identifier);
            for (int c = 0; c < gr->condition_count; c++) {
                fxp_condition_t *cd = &gr->conditions[c];
                printf("     cond[%d] objType=%d num=%d objInfo=%u list=%d "
                       "flags=%02x other=%02x defType=%u id=%d params=%d\n",
                       c, cd->object_type, cd->num, cd->object_info,
                       cd->object_info_list, cd->flags, cd->other_flags,
                       cd->def_type, cd->identifier, cd->param_count);
                for (int p = 0; p < cd->param_count; p++) {
                    fxp_param_t *pa = &cd->params[p];
                    printf("       param code=%d size=%d int=%d expr=%s",
                           pa->code, pa->raw_size, pa->int_val,
                           pa->has_expression ? "Y" : "N");
                    if (pa->has_expression) {
                        printf(" [cmp=%d n=%d:", pa->expression.comparison,
                               pa->expression.count);
                        for (int e = 0; e < pa->expression.count; e++) {
                            fxp_expr_item_t *ei = &pa->expression.items[e];
                            if (ei->object_type == FXP_OT_SYSTEM)
                                printf(" sys%d(%d/%d)", ei->num, ei->int_val,
                                       (int)ei->dbl_val);
                            else
                                printf(" obj%d/info%u/n%d", ei->object_type,
                                       ei->object_info, ei->num);
                        }
                        printf("]");
                    }
                    if (pa->str_val) printf(" str='%s'", pa->str_val);
                    if (pa->code == 14 || pa->code == 44) {
                        printf(" rawkey:");
                        for (int b = 0; b < pa->raw_size && b < 8; b++)
                            printf(" %02x", pa->raw[b]);
                    }
                    printf("\n");
                }
            }
            for (int a = 0; a < gr->action_count; a++) {
                fxp_action_t *ac = &gr->actions[a];
                printf("     act[%d] objType=%d num=%d objInfo=%u list=%d "
                       "flags=%02x other=%02x defType=%u params=%d\n",
                       a, ac->object_type, ac->num, ac->object_info,
                       ac->object_info_list, ac->flags, ac->other_flags,
                       ac->def_type, ac->param_count);
                for (int p = 0; p < ac->param_count; p++) {
                    fxp_param_t *pa = &ac->params[p];
                    printf("       param code=%d size=%d int=%d expr=%s",
                           pa->code, pa->raw_size, pa->int_val,
                           pa->has_expression ? "Y" : "N");
                    if (pa->has_expression) {
                        printf(" [cmp=%d n=%d:", pa->expression.comparison,
                               pa->expression.count);
                        for (int e = 0; e < pa->expression.count; e++) {
                            fxp_expr_item_t *ei = &pa->expression.items[e];
                            if (ei->object_type == FXP_OT_SYSTEM)
                                printf(" sys%d(%d/%d)", ei->num, ei->int_val,
                                       (int)ei->dbl_val);
                            else
                                printf(" obj%d/info%u/n%d", ei->object_type,
                                       ei->object_info, ei->num);
                        }
                        printf("]");
                    }
                    if (pa->str_val) printf(" str='%s'", pa->str_val);
                    if (pa->code == 6 || pa->code == 7 ||
                        pa->code == 35 || pa->code == 36) {
                        printf(" raw:");
                        for (int b = 0; b < pa->raw_size && b < 12; b++)
                            printf(" %02x", pa->raw[b]);
                    }
                    printf("\n");
                }
            }
        }
        printf("  frame chunks: %d\n", fr->chunk_count);
        for (int32_t c = 0; c < fr->chunk_count; c++)
            printf("    chunk id=%u size=%d\n", fr->chunks[c].id, fr->chunks[c].size);
    }

    fxp_mfa_free(mfa);
    free(buf);
    return 0;
}
