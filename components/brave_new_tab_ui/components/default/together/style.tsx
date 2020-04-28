/* This Source Code Form is subject to the terms of the Mozilla Public
 * License. v. 2.0. If a copy of the MPL was not distributed with this file.
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import styled from 'styled-components'
import palette from 'brave-ui/theme/colors'
 
export const WidgetWrapper = styled<{}, 'div'>('div')`
  color: white;
  padding: 6px 20px 12px 20px;
  border-radius: 6px;
  position: relative;
  font-family: ${p => p.theme.fontFamily.body};
  overflow: hidden;
  min-width: 284px;
  min-height: initial;
  background-image: linear-gradient(140deg, #1F2327 0%, #000000 85%);
`

export const Header = styled<{}, 'div'>('div')`
  text-align: left;
`

export const Content = styled<{}, 'div'>('div')`
  margin: 10px 0;
  min-width: 255px;
`

export const WelcomeText = styled<{}, 'span'>('span')`
  display: block;
  font-size: 12px;
  font-weight: 500;
  color: ${palette.white};
`

export const InputLabel = styled<{}, 'span'>('span')`
  display: block;
  font-size: 12px;
  font-weight: 400;
  color: ${palette.white};
`

export const NameInputWrapper = styled<{}, 'div'>('div')`
  height: 30px;
  border: 1px solid rgb(70, 70, 70);
  margin-bottom: 10px;
`

export const NameInputField = styled<{}, 'input'>('input')`
  display: inline-block;
  min-width: 223px;
  height: 32px;
  vertical-align: top;
  border: none;
  color: ${palette.white};
  background: none;
  border: 1px solid rgba(255, 255, 255, 0.5);
  border-radius: 4px;
  padding-left: 5px;

  &:focus {
    outline: 0;
    border: 1px solid rgba(255, 255, 255, 1.0);
  }
`

export const ActionsWrapper = styled<{}, 'div'>('div')`
  margin-bottom: 25px;
  text-align: center;
`

export const CallButton = styled<{}, 'button'>('button')`
  font-size: 14px;
  font-weight: bold;
  border-radius: 20px;
  width: 100%;
  background: ${palette.blurple500};
  border: 0;
  padding: 10px 60px;
  cursor: pointer;
  color: ${palette.white};
  margin-top: 20px;
`

export const TogetherIcon = styled<{}, 'div'>('div')`
  width: 24px;
  height: 24px;
  margin-right: 11px;
  margin-left: -2px;
`

export const StyledTitle = styled<{}, 'div'>('div')`
  margin-top: 6px;
  display: flex;
  justify-content: flex-start;
  align-items: center;
  font-size: 18px;
  font-weight: 600;
  color: ${palette.white};
  font-family: ${p => p.theme.fontFamily.heading};
`

export const StyledTitleText = styled<{}, 'div'>('div')`
  margin-top: 4px;
`
